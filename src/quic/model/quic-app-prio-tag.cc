#include "quic-app-prio-tag.h"
#include "ns3/log.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("QuicAppPrioTag");

QuicAppPrioTag::QuicAppPrioTag ()
  : m_prio (0.5)
{
}

QuicAppPrioTag::QuicAppPrioTag (double prio)
  : m_prio (prio)
{
  SetPrio (prio);
}

QuicAppPrioTag::~QuicAppPrioTag () = default;

TypeId
QuicAppPrioTag::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::QuicAppPrioTag")
    .SetParent<Tag> ()
    .AddConstructor<QuicAppPrioTag> ();
  return tid;
}

TypeId
QuicAppPrioTag::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

void
QuicAppPrioTag::SetPrio (double p)
{
  if (std::isnan (p)) p = 0.5;
  m_prio = std::min (1.0, std::max (0.0, p));
}

double
QuicAppPrioTag::GetPrio () const
{
  return m_prio;
}

uint32_t
QuicAppPrioTag::GetSerializedSize (void) const
{
  return 8; // store double as u64
}

void
QuicAppPrioTag::Serialize (TagBuffer i) const
{
  uint64_t u = 0;
  std::memcpy (&u, &m_prio, sizeof(double));
  i.WriteU64 (u);
}

void
QuicAppPrioTag::Deserialize (TagBuffer i)
{
  uint64_t u = i.ReadU64 ();
  double v = 0.5;
  std::memcpy (&v, &u, sizeof(double));
  SetPrio (v);
}

void
QuicAppPrioTag::Print (std::ostream &os) const
{
  os << "prio=" << m_prio;
}

} // namespace ns3
