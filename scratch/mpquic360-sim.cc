/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * scratch/mpquic360-sim.cc
 *
 * MPQUIC-Ns3 (mpquic-1.2) prototype for 360 GOP-Tile task transmission.
 *
 * Key point (OURS):
 *   We DO NOT set priority on socket/stream objects.
 *   We attach per-frame application priority (0..1) via ns-3 PacketTag (QuicAppPrioTag)
 *   so the priority travels with the packet into TxBuffer/TxScheduler and finally
 *   feeds the MPQUIC path scheduler (PriorityLoad).
 *
 * Modes:
 *   --mode=single : single-path QUIC baseline (EnableMultipath=false)
 *   --mode=rr     : MPQUIC + ROUND_ROBIN path scheduler
 *   --mode=ours   : MPQUIC + PRIORITY_LOAD path scheduler (reads AppPrioTag hint)
 *
 * Task CSV format:
 *   taskId,g,t,k,sizeBytes,deadlineSec,priority,isRedundant,originTaskId,payloadPath,payloadOffset,payloadLen
 *
 * Outputs:
 *   logs/<mode>/sender_tasks.csv
 *   logs/<mode>/receiver_tasks.csv
 *   logs/<mode>/path_stats.csv
 */

 #include "ns3/core-module.h"
 #include "ns3/network-module.h"
 #include "ns3/internet-module.h"
 #include "ns3/point-to-point-module.h"
 #include "ns3/applications-module.h"
 
 #include "ns3/ipv4-address.h"
 #include "ns3/inet-socket-address.h"
 #include "ns3/socket.h"
 #include "ns3/socket-factory.h"
 
//  // QUIC / MPQUIC
//  #include "quic-socket-factory.h"
//  #include "quic-socket-base.h"
//  #include "quic-subheader.h"
//  #include "mp-quic-scheduler.h"
 
//  // Your added PacketTag (name must match your implementation)
//  #include "quic-app-prio-tag.h"

#include "ns3/quic-module.h"
 
 #include <fstream>
 #include <sstream>
 #include <unordered_map>
 #include <vector>
 #include <algorithm>
 #include <filesystem>
 #include <iomanip>
 #include <sys/stat.h>
 #include <string>

 
 using namespace ns3;

 static bool MkDirs(const std::string& path) {
  if (path.empty()) return true;
  if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) return true;
  if (errno != ENOENT) return false;
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos) return false;
  return MkDirs(path.substr(0, pos)) && (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST);
}

std::string GetParentPath(const std::string& path) {
    if (path.empty()) return "";

    std::string p = path;

    // 移除末尾的 '/'
    while (!p.empty() && p.back() == '/') {
        p.pop_back();
    }

    if (p.empty()) return "/"; // 根目录特殊情况

    size_t last_slash = p.find_last_of('/');
    if (last_slash == std::string::npos) {
        return ""; // 无父目录，如 "file.txt"
    } else if (last_slash == 0) {
        return "/"; // 如 "/file.txt" -> "/"
    } else {
        return p.substr(0, last_slash);
    }
}
 
 // ---------------------- helpers ----------------------
 static std::string
 BaseName (const std::string &p)
 {
   auto pos = p.find_last_of ("/\\");
   return (pos == std::string::npos) ? p : p.substr (pos + 1);
 }
 
 static bool
 ReadFileBytes (const std::string &path, uint64_t offset, uint32_t len, std::vector<uint8_t> &out)
 {
   out.assign (len, 0);
   std::ifstream fin (path, std::ios::binary);
   if (!fin.good ())
     return false;
   fin.seekg ((std::streamoff) offset, std::ios::beg);
   fin.read ((char *) out.data (), (std::streamsize) len);
   return fin.gcount () == (std::streamsize) len;
 }
 
 static void
 WriteFileBytesAt (const std::string &path, uint64_t offset, const uint8_t *data, uint32_t len)
 {
   MkDirs (GetParentPath(path));
   std::fstream f (path, std::ios::in | std::ios::out | std::ios::binary);
   if (!f.good ())
     {
       f.open (path, std::ios::out | std::ios::binary);
       f.close ();
       f.open (path, std::ios::in | std::ios::out | std::ios::binary);
     }
   f.seekp ((std::streamoff) offset, std::ios::beg);
   f.write ((const char *) data, (std::streamsize) len);
   f.close ();
 }
 
 // ---------------------- Task ----------------------
 struct Task
 {
   uint32_t taskId = 0;
   uint32_t g = 0, t = 0, k = 0;
   uint32_t sizeBytes = 0;
   double deadlineSec = 0.0;
   double priority = 0.0; // 0..1
   bool isRedundant = false;
   uint32_t originTaskId = 0;
 
   std::string payloadPath;
   uint64_t payloadOffset = 0;
   uint32_t payloadLen = 0;
 };
 
 static std::vector<Task>
 LoadTasksFromCsv (const std::string &path)
 {
   std::vector<Task> tasks;
   std::ifstream fin (path);
   if (!fin.good ())
     {
       NS_FATAL_ERROR ("Cannot open tasks CSV: " << path);
     }
   std::string line;
   while (std::getline (fin, line))
     {
       if (line.empty ())
         continue;
       if (line[0] == '#')
         continue;
       if (line.find ("taskId") != std::string::npos)
         continue;
 
       std::stringstream ss (line);
       std::string tok;
       Task x{};
 
       std::getline (ss, tok, ',');
       x.taskId = std::stoul (tok);
       std::getline (ss, tok, ',');
       x.g = std::stoul (tok);
       std::getline (ss, tok, ',');
       x.t = std::stoul (tok);
       std::getline (ss, tok, ',');
       x.k = std::stoul (tok);
 
       std::getline (ss, tok, ',');
       x.sizeBytes = std::stoul (tok);
       std::getline (ss, tok, ',');
       x.deadlineSec = std::stod (tok);
       std::getline (ss, tok, ',');
       x.priority = std::stod (tok);
 
       std::getline (ss, tok, ',');
       x.isRedundant = (std::stoi (tok) != 0);
       std::getline (ss, tok, ',');
       x.originTaskId = std::stoul (tok);
 
       std::getline (ss, tok, ',');
       x.payloadPath = tok;
       std::getline (ss, tok, ',');
       x.payloadOffset = std::stoull (tok);
       std::getline (ss, tok, ',');
       x.payloadLen = std::stoul (tok);
 
       // clamp
       if (std::isnan (x.priority) || x.priority < 0.0)
         x.priority = 0.0;
       if (x.priority > 1.0)
         x.priority = 1.0;
 
       tasks.push_back (x);
     }
   return tasks;
 }
 
 // ---------------------- Sender Application ----------------------
 class MpQuic360Sender : public Application
 {
 public:
   void Configure (Ipv4Address bindIp,
                   Ipv4Address peerIp,
                   uint16_t port,
                   std::string tasksCsv,
                   std::string outDir,
                   std::string mode,
                   uint32_t chunkSize)
   {
     m_bindIp = bindIp;
     m_peerIp = peerIp;
     m_port = port;
     m_tasksCsv = std::move (tasksCsv);
     m_outDir = std::move (outDir);
     m_mode = std::move (mode);
     m_chunkSize = chunkSize;
   }
 
 private:
   void StartApplication () override
   {
     MkDirs (m_outDir);
     m_tasks = LoadTasksFromCsv (m_tasksCsv);
 
     // originals first, then redundant; within each, by priority desc
     std::stable_sort (m_tasks.begin (), m_tasks.end (),
                       [] (const Task &a, const Task &b)
                       {
                         if (a.isRedundant != b.isRedundant)
                           return a.isRedundant < b.isRedundant;
                         return a.priority > b.priority;
                       });
 
     // Create QUIC socket via factory
     m_sock = Socket::CreateSocket (GetNode (), QuicSocketFactory::GetTypeId ());
     NS_ASSERT (m_sock);
 
     // Bind to a specific local IP (first path)
     InetSocketAddress local (m_bindIp, 0);
     if (m_sock->Bind (local) != 0)
       {
         NS_FATAL_ERROR ("Sender Bind failed");
       }
 
     // Connect
     InetSocketAddress remote (m_peerIp, m_port);
     if (m_sock->Connect (remote) != 0)
       {
         NS_FATAL_ERROR ("Sender Connect failed");
       }
 
     // Enable/disable multipath
     Ptr<QuicSocketBase> base = DynamicCast<QuicSocketBase> (m_sock);
     if (!base)
       {
         NS_FATAL_ERROR ("Sender socket is not QuicSocketBase");
       }
 
     if (m_mode == "single")
       {
         base->SetAttribute ("EnableMultipath", BooleanValue (false));
       }
     else
       {
         base->SetAttribute ("EnableMultipath", BooleanValue (true));
       }
 
     // log files
     m_fSend.open (m_outDir + "/sender_tasks.csv", std::ios::out);
     m_fSend << "simTime,taskId,g,t,k,isRedundant,originTaskId,priority,deadlineSec,streamId,bytesSent\n";
 
     m_fPath.open (m_outDir + "/path_stats.csv", std::ios::out);
     m_fPath << "simTime,subflowId,lastRttMs,cWnd,bytesInFlight,availableWindow\n";
 
     Simulator::Schedule (Seconds (0.05), &MpQuic360Sender::SamplePathStats, this);
     Simulator::Schedule (Seconds (0.10), &MpQuic360Sender::SendNextTask, this);
   }
 
   void StopApplication () override
   {
     if (m_fSend.is_open ())
       m_fSend.close ();
     if (m_fPath.is_open ())
       m_fPath.close ();
     if (m_sock)
       m_sock->Close ();
   }
 
   void SamplePathStats ()
{
  Ptr<QuicSocketBase> base = DynamicCast<QuicSocketBase> (m_sock);
  if (!base)
    {
      Simulator::Schedule (Seconds (0.5), &MpQuic360Sender::SamplePathStats, this);
      return;
    }

  auto subflows = base->GetActiveSubflows ();
  if (subflows.empty ())
    {
      // 连接/子流尚未建立，不采样
      Simulator::Schedule (Seconds (0.5), &MpQuic360Sender::SamplePathStats, this);
      return;
    }

  for (uint32_t i = 0; i < subflows.size (); i++)
    {
      // 关键：subflow 可能是空
      if (subflows[i] == 0)
        {
          continue;
        }

      // 关键：TCB 可能尚未创建
      auto tcb = subflows[i]->m_tcb;
      if (tcb == 0)
        {
          continue;
        }

      double rttMs = tcb->m_lastRtt.Get ().GetSeconds () * 1000.0;
      uint32_t cwnd = tcb->m_cWnd.Get ();
      uint32_t bif = tcb->m_bytesInFlight.Get ();

      // AvailableWindow(i) 在某些实现里也可能要求内部结构ready
      // 如果你后面还崩，就先把 aw 这一列写成 0 或者 try-catch式防御（ns3没异常）
      uint32_t aw = 0;
      // 更保守：只有当 base 内部认为该 subflow 可用才调用
      // （如果你库里有 IsSubflowActive(i) / GetNSubflows() 之类接口就用它）
      aw = base->AvailableWindow (i);

      m_fPath << std::fixed << std::setprecision (6)
              << Simulator::Now ().GetSeconds () << ","
              << i << "," << rttMs << ","
              << cwnd << "," << bif << "," << aw
              << "\n";
    }

  Simulator::Schedule (Seconds (0.05), &MpQuic360Sender::SamplePathStats, this);
}

 
   void SendNextTask ()
   {
     if (!m_sock)
       return;
     if (m_idx >= m_tasks.size ())
       return;
 
     // Set MP path scheduler type (global default; do it once is enough,
     // but doing here is harmless)
     if (m_mode == "ours")
       {
         Config::SetDefault ("ns3::MpQuicScheduler::SchedulerType",
                             IntegerValue ((int) MpQuicScheduler::PRIORITY_LOAD));
       }
     else if (m_mode == "rr")
       {
         Config::SetDefault ("ns3::MpQuicScheduler::SchedulerType",
                             IntegerValue ((int) MpQuicScheduler::ROUND_ROBIN));
       }
     else
       {
         Config::SetDefault ("ns3::MpQuicScheduler::SchedulerType",
                             IntegerValue ((int) MpQuicScheduler::MIN_RTT));
       }
 
     const Task &u = m_tasks[m_idx];
 
     // One task = one stream
     // Avoid stream 0; use client-initiated bidirectional style (common) => 4,8,12,...
     // Keep mapping reversible for receiver:
     //   streamId = 4*(taskId+1)  =>  taskId = streamId/4 - 1
     const uint32_t streamId = 4 * (u.taskId + 1);
 
     uint32_t remaining = u.sizeBytes;
     uint64_t fileOff = u.payloadOffset;
     uint32_t sentTotal = 0;
 
     while (remaining > 0)
       {
         uint32_t cs = std::min (m_chunkSize, remaining);
 
         std::vector<uint8_t> buf;
         if (!ReadFileBytes (u.payloadPath, fileOff, cs, buf))
           {
             buf.assign (cs, 0);
           }
 
         Ptr<Packet> p = Create<Packet> (buf.data (), buf.size ());
 
         // ---- KEY: attach application priority hint to THIS frame ----
         // Your QuicAppPrioTag should store a double (0..1) or quantized int.
         QuicAppPrioTag tag;
         tag.SetPrio (u.priority);
         p->AddPacketTag (tag);
 
         // Send using ns-3 Socket::Send(packet, flags) where flags carries streamId in this QUIC implementation.
         // In mpquic-1.2, QuicSocketBase::Send(p, flags) forwards flags into QuicL5Protocol::DispatchSend(..., flags).
         int ret = m_sock->Send (p, streamId);
         if (ret < 0)
           {
            //  NS_LOG_WARN ("Send failed (streamId=" << streamId << ")");
           }
 
         remaining -= cs;
         fileOff += cs;
         sentTotal += cs;
       }
 
     m_fSend << std::fixed << std::setprecision (6)
             << Simulator::Now ().GetSeconds () << ","
             << u.taskId << "," << u.g << "," << u.t << "," << u.k << ","
             << (u.isRedundant ? 1 : 0) << "," << u.originTaskId << ","
             << u.priority << "," << u.deadlineSec << ","
             << streamId << "," << sentTotal
             << "\n";
 
     m_idx++;
     Simulator::Schedule (MilliSeconds (1), &MpQuic360Sender::SendNextTask, this);
   }
 
 private:
   Ipv4Address m_bindIp;
   Ipv4Address m_peerIp;
   uint16_t m_port = 4433;
   std::string m_tasksCsv;
   std::string m_outDir;
   std::string m_mode = "ours";
   uint32_t m_chunkSize = 1200;
 
   std::vector<Task> m_tasks;
   size_t m_idx = 0;
 
   Ptr<Socket> m_sock;
   std::ofstream m_fSend;
   std::ofstream m_fPath;
 };
 
 // ---------------------- Receiver Application ----------------------
 class MpQuic360Receiver : public Application
 {
 public:
   void Configure (Ipv4Address bindIp,
                   uint16_t port,
                   std::string tasksCsv,
                   std::string outDir,
                   std::string recoverDir,
                   bool writeRecovered)
   {
     m_bindIp = bindIp;
     m_port = port;
     m_tasksCsv = std::move (tasksCsv);
     m_outDir = std::move (outDir);
     m_recoverDir = std::move (recoverDir);
     m_writeRecovered = writeRecovered;
   }
 
 private:
   struct Meta
   {
     uint32_t g, t, k;
     double deadline;
     double priority;
     bool isRed;
     uint32_t originId;
     std::string payloadPath;
     uint64_t payloadOffset;
     uint32_t payloadLen;
     uint32_t sizeBytes;
   };
 
   struct Prog
   {
     uint32_t got = 0;
     double firstRx = -1;
     double lastRx = -1;
     bool done = false;
     bool miss = false;
   };
 
   void StartApplication () override
   {
     MkDirs (m_outDir);
     MkDirs (m_recoverDir);
 
     // load meta map by taskId
     auto tasks = LoadTasksFromCsv (m_tasksCsv);
     for (auto &t : tasks)
       {
         Meta m;
         m.g = t.g;
         m.t = t.t;
         m.k = t.k;
         m.deadline = t.deadlineSec;
         m.priority = t.priority;
         m.isRed = t.isRedundant;
         m.originId = t.originTaskId;
         m.payloadPath = t.payloadPath;
         m.payloadOffset = t.payloadOffset;
         m.payloadLen = t.payloadLen;
         m.sizeBytes = t.sizeBytes;
         m_meta[t.taskId] = m;
       }
 
     // Create QUIC server socket
     m_sock = Socket::CreateSocket (GetNode (), QuicSocketFactory::GetTypeId ());
     NS_ASSERT (m_sock);
 
     InetSocketAddress local (m_bindIp, m_port);
     if (m_sock->Bind (local) != 0)
       {
         NS_FATAL_ERROR ("Receiver Bind failed");
       }
 
     // QUIC server side typically needs Listen
     m_sock->Listen ();
 
     m_sock->SetRecvCallback (MakeCallback (&MpQuic360Receiver::HandleRead, this));
 
     m_fRecv.open (m_outDir + "/receiver_tasks.csv", std::ios::out);
     m_fRecv << "simTime,taskId,g,t,k,isRedundant,originTaskId,priority,deadlineSec,streamId,bytesRx,totalBytes,completed,deadlineMiss,firstRx,lastRx\n";
   }
 
   void StopApplication () override
   {
     if (m_fRecv.is_open ())
       m_fRecv.close ();
     if (m_sock)
       m_sock->Close ();
   }
 
   void HandleRead (Ptr<Socket> sock)
   {
     Address from;
     Ptr<Packet> p;
     while ((p = sock->RecvFrom (from)))
       {
         if (p->GetSize () == 0)
           continue;
 
         // QUIC frame carries QuicSubheader; use it to get streamId.
         QuicSubheader qsb;
         uint32_t headerSize = p->PeekHeader (qsb);
         if (headerSize == 0 || !qsb.IsStream ())
           {
             continue;
           }
 
         const uint32_t streamId = qsb.GetStreamId ();
         const uint32_t n = p->GetSize ();
 
         // Reverse mapping: taskId = streamId/4 - 1
         if (streamId < 4)
           continue;
         const uint32_t taskId = streamId / 4 - 1;
 
         auto it = m_meta.find (taskId);
         if (it == m_meta.end ())
           continue;
 
         const Meta &m = it->second;
         Prog &pr = m_prog[taskId];
 
         double now = Simulator::Now ().GetSeconds ();
         if (pr.firstRx < 0)
           pr.firstRx = now;
         pr.lastRx = now;
         pr.got += n;
 
         if (!pr.done && pr.got >= m.sizeBytes)
           {
             pr.done = true;
             pr.miss = (now > m.deadline);
           }
 
         // Optional: write recovered bytes (original tasks only)
         if (m_writeRecovered && (!m.isRed))
           {
             // We remove header to get payload bytes
             QuicSubheader tmp;
             p->RemoveHeader (tmp);
 
             std::vector<uint8_t> buf (p->GetSize ());
             p->CopyData (buf.data (), buf.size ());
 
             // sequential write into slice region
             uint64_t off = m.payloadOffset + (uint64_t) (pr.got - n);
             std::string outFile = m_recoverDir + "/" + BaseName (m.payloadPath);
             WriteFileBytesAt (outFile, off, buf.data (), (uint32_t) buf.size ());
           }
 
         m_fRecv << std::fixed << std::setprecision (6)
                 << now << ","
                 << taskId << ","
                 << m.g << "," << m.t << "," << m.k << ","
                 << (m.isRed ? 1 : 0) << ","
                 << m.originId << ","
                 << m.priority << ","
                 << m.deadline << ","
                 << streamId << ","
                 << n << ","
                 << m.sizeBytes << ","
                 << (pr.done ? 1 : 0) << ","
                 << (pr.miss ? 1 : 0) << ","
                 << pr.firstRx << ","
                 << pr.lastRx
                 << "\n";
       }
   }
 
 private:
   Ipv4Address m_bindIp;
   uint16_t m_port = 4433;
   std::string m_tasksCsv;
   std::string m_outDir;
   std::string m_recoverDir;
   bool m_writeRecovered = false;
 
   Ptr<Socket> m_sock;
   std::ofstream m_fRecv;
 
   std::unordered_map<uint32_t, Meta> m_meta;
   std::unordered_map<uint32_t, Prog> m_prog;
 };
 
 // ---------------------- Main ----------------------
 int
 main (int argc, char *argv[])
 {
   std::string mode = "ours"; // single|rr|ours
 
   std::string rate0 = "50Mbps", rate1 = "20Mbps";
   std::string delay0 = "20ms", delay1 = "60ms";
   double loss0 = 0.001, loss1 = 0.02;
 
   double simTime = 20.0;
 
   std::string tasksCsv = "data/tasks/tasks.csv";
   std::string outBase = "logs";
   std::string recoverBase = "data/recovered";
   bool writeRecovered = false;
 
   uint32_t chunkSize = 1200;
 
   CommandLine cmd;
   cmd.AddValue ("mode", "single|rr|ours", mode);
   cmd.AddValue ("rate0", "Path0 rate", rate0);
   cmd.AddValue ("rate1", "Path1 rate", rate1);
   cmd.AddValue ("delay0", "Path0 delay", delay0);
   cmd.AddValue ("delay1", "Path1 delay", delay1);
   cmd.AddValue ("loss0", "Path0 loss", loss0);
   cmd.AddValue ("loss1", "Path1 loss", loss1);
   cmd.AddValue ("simTime", "Simulation time", simTime);
 
   cmd.AddValue ("tasksCsv", "Tasks CSV path", tasksCsv);
   cmd.AddValue ("outBase", "Output base dir", outBase);
   cmd.AddValue ("recoverBase", "Recovered output base dir", recoverBase);
   cmd.AddValue ("writeRecovered", "Write recovered bytes for original tasks", writeRecovered);
 
   cmd.AddValue ("chunkSize", "Chunk size per send", chunkSize);
   cmd.Parse (argc, argv);
 
   std::string outDir = outBase + "/" + mode;
   std::string recoverDir = recoverBase + "/" + mode;
 
   NS_LOG_UNCOND ("mpquic360-sim START mode=" << mode);
   NS_LOG_UNCOND (" tasksCsv=" << tasksCsv);
   NS_LOG_UNCOND (" outDir=" << outDir);
 
   NodeContainer nodes;
   nodes.Create (2); // 0 sender, 1 receiver
 
   PointToPointHelper p2p0, p2p1;
   p2p0.SetDeviceAttribute ("DataRate", StringValue (rate0));
   p2p0.SetChannelAttribute ("Delay", StringValue (delay0));
   p2p1.SetDeviceAttribute ("DataRate", StringValue (rate1));
   p2p1.SetChannelAttribute ("Delay", StringValue (delay1));
 
   NetDeviceContainer dev0 = p2p0.Install (nodes.Get (0), nodes.Get (1));
   NetDeviceContainer dev1 = p2p1.Install (nodes.Get (0), nodes.Get (1));
 
   Ptr<RateErrorModel> em0 = CreateObject<RateErrorModel> ();
   em0->SetAttribute ("ErrorRate", DoubleValue (loss0));
   dev0.Get (1)->SetAttribute ("ReceiveErrorModel", PointerValue (em0));
 
   Ptr<RateErrorModel> em1 = CreateObject<RateErrorModel> ();
   em1->SetAttribute ("ErrorRate", DoubleValue (loss1));
   dev1.Get (1)->SetAttribute ("ReceiveErrorModel", PointerValue (em1));
 
  //  InternetStackHelper internet;
  //  internet.Install (nodes);

   QuicHelper quic;
   quic.InstallQuic(nodes);
 
   Ipv4AddressHelper ipv4;
 
   ipv4.SetBase ("10.0.0.0", "255.255.255.0");
   Ipv4InterfaceContainer if0 = ipv4.Assign (dev0);
 
   ipv4.SetBase ("10.0.1.0", "255.255.255.0");
   Ipv4InterfaceContainer if1 = ipv4.Assign (dev1);
 
   Ipv4Address senderIp0 = if0.GetAddress (0);
   Ipv4Address recvIp0 = if0.GetAddress (1);
 
   // second interface IPs (kept for MPQUIC path manager to discover)
   (void) if1;
 
   uint16_t port = 4433;
 
   Ptr<MpQuic360Receiver> rx = CreateObject<MpQuic360Receiver> ();
   rx->Configure (recvIp0, port, tasksCsv, outDir, recoverDir, writeRecovered);
   nodes.Get (1)->AddApplication (rx);
   rx->SetStartTime (Seconds (0.0));
   rx->SetStopTime (Seconds (simTime));
 
   Ptr<MpQuic360Sender> tx = CreateObject<MpQuic360Sender> ();
   tx->Configure (senderIp0, recvIp0, port, tasksCsv, outDir, mode, chunkSize);
   nodes.Get (0)->AddApplication (tx);
   tx->SetStartTime (Seconds (0.10));
   tx->SetStopTime (Seconds (simTime));
 
   Simulator::Stop (Seconds (simTime));
   Simulator::Run ();
   Simulator::Destroy ();
 
   NS_LOG_UNCOND ("mpquic360-sim DONE mode=" << mode);
   return 0;
 }
 