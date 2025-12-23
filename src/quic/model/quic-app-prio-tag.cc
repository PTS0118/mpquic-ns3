#include "quic-app-prio-tag.h"
#include "ns3/type-id.h"

namespace ns3 {

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
QuicAppPrioTag::Print (std::ostream &os) const
{
  os << "prio=" << GetPriority ();
}

} // namespace ns3
