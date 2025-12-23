/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2022 Pan Lab, Department of Computer Science, University of Victoria
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Shengjie Shu <shengjies@uvic.ca>
 */


#include "ns3/object.h"
#include "ns3/log.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/boolean.h"
#include "ns3/trace-source-accessor.h"
#include "ns3/nstime.h"
#include "quic-stream.h"
#include "ns3/node.h"
#include "ns3/string.h"

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <vector>
#include <bitset>
#include <numeric>

#include "mp-quic-scheduler.h"
#include "ns3/random-variable-stream.h"
#include "quic-socket-base.h"


using Eigen::MatrixXd;
using Eigen::VectorXd;

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("MpQuicScheduler");

NS_OBJECT_ENSURE_REGISTERED (MpQuicScheduler);


TypeId
MpQuicScheduler::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::MpQuicScheduler")
    .SetParent<Object> ()
    .SetGroupName ("Internet")
    .AddAttribute ("SchedulerType",
                   "define the type of the scheduler",
                   IntegerValue (MIN_RTT),
                   MakeIntegerAccessor (&MpQuicScheduler::m_schedulerType),
                   MakeIntegerChecker<int16_t> ())
    .AddAttribute ("MabRate",
                   "define the rate of the MAB scheduler",
                   UintegerValue (100),
                   MakeUintegerAccessor (&MpQuicScheduler::m_rate),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("BlestLambda",
                   "define the lambda of the BLEST",
                   UintegerValue (1000),
                   MakeUintegerAccessor (&MpQuicScheduler::m_lambda),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("BlestVar",
                   "define the lambda of the BLEST",
                   UintegerValue (100),
                   MakeUintegerAccessor (&MpQuicScheduler::m_bVar),
                   MakeUintegerChecker<uint16_t> ())
    .AddAttribute ("Select",
                   "string of select",
                   UintegerValue (0),
                   MakeUintegerAccessor (&MpQuicScheduler::m_select),
                   MakeUintegerChecker<uint16_t> ())            
     
  ;
  return tid;
}

MpQuicScheduler::MpQuicScheduler ()
  : Object (),
  m_socket(0),
  m_lastUsedPathId(0),
  m_select(0)
{
  NS_LOG_FUNCTION_NOARGS ();
  m_lastUpdateRounds = 1;
  m_e = 0;
}

MpQuicScheduler::~MpQuicScheduler ()
{
  NS_LOG_FUNCTION_NOARGS ();
}



std::vector<double> 
MpQuicScheduler::GetNextPathIdToUse()
{
  m_subflows = m_socket->GetActiveSubflows();
  std::vector<double> tosend(m_subflows.size(), 0.0);
  if (m_subflows.empty())
  {
    tosend.push_back(1.0);
    return tosend;
  }
  switch (m_schedulerType)
  {
    case ROUND_ROBIN:
      tosend = RoundRobin();
      break;

    case MIN_RTT:
      tosend = MinRtt();
      break;
    
    case BLEST:
      tosend = Blest();
      break;

    case ECF:
      tosend = Ecf();
      break;

    case PEEKABOO:
      tosend = Peekaboo();
      break;
    
    case PRIORITY_LOAD:
      tosend = PriorityLoad();
      break;

    default:
      tosend = RoundRobin();
      break;
      
  }

  return tosend;
}

std::vector<double>
MpQuicScheduler::RoundRobin()
{
  std::vector<double> tosend(m_subflows.size(), 0.0);
  if (m_subflows.size() <= 1){
    m_lastUsedPathId = 0;
    tosend[m_lastUsedPathId] = 1.0;
    return tosend;
  }

  m_lastUsedPathId = (m_lastUsedPathId + 1) % m_subflows.size();

  tosend[m_lastUsedPathId] = 1.0;
  return tosend;
}

std::vector<double>
MpQuicScheduler::MinRtt()
{
  NS_LOG_FUNCTION (this);
  std::vector<double> tosend(m_subflows.size(), 0.0);

  if (m_subflows.size() <= 1){
    m_lastUsedPathId = 0;
    tosend[m_lastUsedPathId] = 1.0;
    return tosend;
  }

  if (m_subflows[1]->m_tcb->m_lastRtt.Get().GetSeconds() == 0) {
    m_lastUsedPathId = 1;
    tosend[m_lastUsedPathId] = 1.0;
    return tosend;
  }

  Time rttS;
  Time rttF;
  uint8_t fastPathId = 1;
  uint8_t slowPathId = 0;

  if (m_subflows[0]->m_tcb->m_lastRtt >= m_subflows[1]->m_tcb->m_lastRtt){
    rttS = m_subflows[0]->m_tcb->m_lastRtt;
    rttF = m_subflows[1]->m_tcb->m_lastRtt;
    slowPathId = 0;
    fastPathId = 1;
  } else {
    rttS = m_subflows[1]->m_tcb->m_lastRtt;
    rttF = m_subflows[0]->m_tcb->m_lastRtt;
    slowPathId = 1;
    fastPathId = 0;
  }

  if (m_socket->AvailableWindow (fastPathId) > 0){
    m_lastUsedPathId = fastPathId;
  }else {
    m_lastUsedPathId = slowPathId;
  }

  tosend[m_lastUsedPathId] = 1.0;
  return tosend;
}

std::vector<double>
MpQuicScheduler::PriorityLoad()
{
  m_subflows = m_socket->GetActiveSubflows();
  const uint32_t K = (uint32_t)m_subflows.size();
  std::vector<double> w(K, 0.0);

  if (K <= 1)
    {
      w[0] = 1.0;
      return w;
    }

  // 1) 当前要发的“应用优先级 hint”
  double prio = 0.5;
  if (m_socket)
    {
      prio = m_socket->GetTxPriorityHint(); // ✅ 你刚在 QuicSocketBase 加的接口
    }
  if (prio < 0.0) prio = 0.0;
  if (prio > 1.0) prio = 1.0;

  // 2) 基于路径状态打分：rtt 越小越好、available window 越大越好、inflight 越小越好
  std::vector<double> score(K, 0.0);

  double rttMin = 1e9, rttMax = 0.0;
  for (uint32_t i=0;i<K;i++)
    {
      double rtt = m_subflows[i]->m_tcb->m_lastRtt.Get().GetSeconds();
      if (rtt <= 0.0) rtt = 1e-3;
      rttMin = std::min(rttMin, rtt);
      rttMax = std::max(rttMax, rtt);
    }
  double span = std::max(1e-6, rttMax - rttMin);

  for (uint32_t i=0;i<K;i++)
    {
      double rtt = m_subflows[i]->m_tcb->m_lastRtt.Get().GetSeconds();
      if (rtt <= 0.0) rtt = 1e-3;
      double rttBenefit = 1.0 - (rtt - rttMin) / span; // 0..1

      double wnd = (double)m_socket->AvailableWindow(i);
      double infl = (double)m_socket->BytesInFlight(i);

      // 轻量化：log 压缩动态范围
      double wndTerm  = std::log(1.0 + wnd);
      double inflTerm = std::log(1.0 + infl);

      score[i] = 1.0 * rttBenefit + 0.3 * wndTerm - 0.3 * inflTerm;
    }

  // 3) softmax -> 权重；prio 越高，温度越低，越“集中到最好路径”
  uint32_t best = 0;
  for (uint32_t i=1;i<K;i++)
    if (score[i] > score[best]) best = i;

  double temp = std::max(0.15, 1.0 - 0.85 * prio);

  double sum = 0.0;
  for (uint32_t i=0;i<K;i++)
    {
      double z = (score[i] - score[best]) / temp;
      double e = std::exp(z);
      w[i] = e;
      sum += e;
    }
  if (sum <= 0.0)
    {
      w.assign(K, 0.0);
      w[best] = 1.0;
      return w;
    }
  for (uint32_t i=0;i<K;i++) w[i] /= sum;

  return w;
}


void
MpQuicScheduler::SetSocket(Ptr<QuicSocketBase> sock)
{
  NS_LOG_FUNCTION (this);
  m_socket = sock;
}


std::vector<double>
MpQuicScheduler::Blest() //only allow two subflows
{
  NS_LOG_FUNCTION (this);
  std::vector<double> tosend(m_subflows.size(), 0.0);

  if (m_subflows.size() <= 1){
    m_lastUsedPathId = 0;
    tosend[m_lastUsedPathId] = 1.0;
    return tosend;
  }

  if (m_subflows[1]->m_tcb->m_lastRtt.Get().GetSeconds() == 0) {
    m_lastUsedPathId = 1;
    tosend[m_lastUsedPathId] = 1.0;
    return tosend;
  }
  
  Time rttS;
  Time rttF;
  uint8_t fastPathId = 1;
  uint8_t slowPathId = 0;
  uint32_t mss = m_socket->GetSegSize();

  if (m_subflows[0]->m_tcb->m_lastRtt > m_subflows[1]->m_tcb->m_lastRtt){
    rttS = m_subflows[0]->m_tcb->m_lastRtt;
    rttF = m_subflows[1]->m_tcb->m_lastRtt;
    slowPathId = 0;
    fastPathId = 1;
  } else {
    rttS = m_subflows[1]->m_tcb->m_lastRtt;
    rttF = m_subflows[0]->m_tcb->m_lastRtt;
    slowPathId = 1;
    fastPathId = 0;
  }

  if (m_socket->AvailableWindow (fastPathId) > 0){
    m_lastUsedPathId = fastPathId;
  } else {
    double_t rtts = rttS.GetSeconds()/rttF.GetSeconds();
    double_t cwndF = m_subflows[fastPathId]->m_tcb->m_cWnd/mss;
    double_t X = mss * (cwndF + (rtts-1)/2) * rtts;
    double_t comp = m_socket->GetTxAvailable() - (m_socket->BytesInFlight(slowPathId)+mss);
    m_lambda = m_lambda + m_bVar;
    if(X * m_lambda > comp) { //not send on slow path
      m_lastUsedPathId = fastPathId;
    } else {
      m_lastUsedPathId = slowPathId;
    }
  }
  
  tosend[m_lastUsedPathId] = 1.0;
  return tosend;
}


std::vector<double>
MpQuicScheduler::Ecf() //only allow two subflows
{
  NS_LOG_FUNCTION (this);
  std::vector<double> tosend(m_subflows.size(), 0.0);
  if (m_subflows.size() <= 1){
    m_lastUsedPathId = 0;
    tosend[m_lastUsedPathId] = 1.0;
    return tosend;
  }

  if (m_subflows[1]->m_tcb->m_lastRtt.Get().GetSeconds() == 0) {
    m_lastUsedPathId = (m_lastUsedPathId + 1) % m_subflows.size();
    tosend[m_lastUsedPathId] = 1.0;
    return tosend;
  } 
  
  
  Time rttS;
  Time rttF;
  uint8_t fastPathId = 1;
  uint8_t slowPathId = 0;

  if (m_subflows[0]->m_tcb->m_lastRtt > m_subflows[1]->m_tcb->m_lastRtt){
    rttS = m_subflows[0]->m_tcb->m_lastRtt;
    rttF = m_subflows[1]->m_tcb->m_lastRtt;
    slowPathId = 0;
    fastPathId = 1;
  } else {
    rttS = m_subflows[1]->m_tcb->m_lastRtt;
    rttF = m_subflows[0]->m_tcb->m_lastRtt;
    slowPathId = 1;
    fastPathId = 0;
  }

  if (m_socket->AvailableWindow (fastPathId) > 0){
    m_lastUsedPathId = fastPathId;
  }else {
    uint32_t k = m_socket->GetBytesInBuffer();
    double n = 1 + k/m_subflows[fastPathId]->m_tcb->m_cWnd.Get();
    double delta = max(m_subflows[fastPathId]->m_tcb->m_rttVar.GetSeconds(),m_subflows[slowPathId]->m_tcb->m_rttVar.GetSeconds());
    if (n*rttF.GetSeconds() < (1+m_waiting*1)*(rttS.GetSeconds()+delta)){
      if (k/m_subflows[slowPathId]->m_tcb->m_cWnd.Get() * rttS.GetSeconds() >= 2*rttF.GetSeconds()+delta){
        m_waiting = 1;
        m_lastUsedPathId = fastPathId;
        tosend[m_lastUsedPathId] = 1.0;
        return tosend;
      } else {
        m_lastUsedPathId = slowPathId;
      }
    } else {
      m_waiting = 0;
      m_lastUsedPathId = slowPathId;
    }
  }  

  tosend[m_lastUsedPathId] = 1.0;
  return tosend;
}

std::vector<double>
MpQuicScheduler::Peekaboo()
{
  NS_LOG_FUNCTION (this);
  uint8_t K = m_subflows.size();
  if(EPR.size() < K)
  {
    EPR.push_back(0.0);
    A.push_back(MatrixXd::Identity(6,6));
    b.push_back(VectorXd::Constant(6,0));
  }
  std::vector<double> tosend(m_subflows.size(), 0.0);

  if (m_subflows.size() <= 1){
    m_lastUsedPathId = 0;
    tosend[m_lastUsedPathId] = 1.0;
    return tosend;
  }
  if (m_subflows[1]->m_tcb->m_lastRtt.Get().GetSeconds() == 0) {
    m_lastUsedPathId = 1;
    tosend[m_lastUsedPathId] = 1.0;
    return tosend;
  }

  Time rttS;
  Time rttF;
  uint8_t fastPathId = 1;
  uint8_t slowPathId = 0;

  if (m_subflows[0]->m_tcb->m_lastRtt >= m_subflows[1]->m_tcb->m_lastRtt){
    rttS = m_subflows[0]->m_tcb->m_lastRtt;
    rttF = m_subflows[1]->m_tcb->m_lastRtt;
    slowPathId = 0;
    fastPathId = 1;
  } else {
    rttS = m_subflows[1]->m_tcb->m_lastRtt;
    rttF = m_subflows[0]->m_tcb->m_lastRtt;
    slowPathId = 1;
    fastPathId = 0;
  }

  if (m_socket->AvailableWindow (fastPathId) > 0){
    m_lastUsedPathId = fastPathId;
  }else {
    for (int i = 0; i < 2; i++){
      MatrixXd zeta = A[i]*b[i];
      EPR[i] = (peek_x.transpose() * zeta).value() + 0.8 * std::sqrt(peek_x.transpose() * A[i].inverse() * peek_x);
    }

    if(EPR[fastPathId] > EPR[slowPathId]){
      m_lastUsedPathId = fastPathId; //wait
    } else {
      m_lastUsedPathId = slowPathId; //transmit on slow path
    }

    A[m_lastUsedPathId] = A[m_lastUsedPathId] + peek_x * peek_x.transpose();
    b[m_lastUsedPathId] = b[m_lastUsedPathId] + R * peek_x;

  }

  tosend[m_lastUsedPathId] = 1.0;
  return tosend;

}


void
MpQuicScheduler::PeekabooReward(uint8_t pathId, Time lastActTime)
{
  NS_LOG_FUNCTION (this);
  
  rtt[pathId] = m_subflows[pathId]->m_tcb->m_lastRtt.Get().GetDouble();
  if (rtt[0]==0) rtt[0] = 10;     // initialize rtt0 with 20ms
  if (rtt[1]==0) rtt[1] = 10;     // initialize rtt0 with 20ms
  if(pathId == 0){
    peek_x[0] = m_subflows[pathId]->m_tcb->m_cWnd.Get()/rtt[pathId];
    peek_x[1] = m_subflows[pathId]->m_tcb->m_bytesInFlight.Get()/rtt[pathId];
    peek_x[2] = m_subflows[pathId]->m_tcb->m_cWnd.Get()/rtt[pathId];
  } else{
    peek_x[3] = m_subflows[pathId]->m_tcb->m_cWnd.Get()/rtt[pathId];
    peek_x[4] = m_subflows[pathId]->m_tcb->m_bytesInFlight.Get()/rtt[pathId];
    peek_x[5] = m_subflows[pathId]->m_tcb->m_cWnd.Get()/rtt[pathId];
  }


  double rtt_f = std::min(rtt[0], rtt[1]);
  double rtt_s = std::max(rtt[0], rtt[1]);

  T_r = std::max(2*rtt_f, rtt_s);
  T_e = (Now () - lastActTime).GetMilliSeconds();
  if (T_e < 3 * T_r)
    {
      double r = 1460 * 1000 * 1e9/ (Now() - lastActTime).GetDouble();
      R = R + r * g;
      if (T_e <= T_r)
        {
          g = 0.9 * g;
        }
      else if (T_e <= 2 * T_r)
        {
          g = 0.7 * g;
        }
      else
        {
          g = 0.5 * g;
        }
    }
  
  
}


void
MpQuicScheduler::SetNumOfLostPackets(uint16_t lost){
  m_lostPackets = lost;
}


} // namespace ns3
