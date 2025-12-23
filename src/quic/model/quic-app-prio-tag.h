#ifndef QUIC_APP_PRIO_TAG_H
#define QUIC_APP_PRIO_TAG_H

#include "ns3/tag.h"
#include "ns3/uinteger.h"

namespace ns3 {

/**
 * PacketTag to carry application priority hint (0..1) for a QUIC STREAM frame.
 * We store it as uint16 scaled to [0, 65535] to keep serialization simple.
 */
class QuicAppPrioTag : public Tag
{
public:
  QuicAppPrioTag () : m_scaled (32768) {}
  explicit QuicAppPrioTag (double prio) { SetPriority (prio); }

  static TypeId GetTypeId (void);
  TypeId GetInstanceTypeId (void) const override;

  uint32_t GetSerializedSize (void) const override { return 2; }
  void Serialize (TagBuffer i) const override { i.WriteU16 (m_scaled); }
  void Deserialize (TagBuffer i) override { m_scaled = i.ReadU16 (); }
  void Print (std::ostream &os) const override;

  void SetPriority (double prio)
  {
    if (prio < 0.0) prio = 0.0;
    if (prio > 1.0) prio = 1.0;
    m_scaled = (uint16_t) (prio * 65535.0 + 0.5);
  }

  double GetPriority () const { return ((double)m_scaled) / 65535.0; }

private:
  uint16_t m_scaled;
};

} // namespace ns3

#endif // QUIC_APP_PRIO_TAG_H