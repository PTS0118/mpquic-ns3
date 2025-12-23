#ifndef QUIC_APP_PRIO_TAG_H
#define QUIC_APP_PRIO_TAG_H

#include "ns3/tag.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"

namespace ns3 {

class QuicAppPrioTag : public Tag
{
public:
  QuicAppPrioTag ();
  explicit QuicAppPrioTag (double prio);

  static TypeId GetTypeId (void);
  TypeId GetInstanceTypeId (void) const override;

  uint32_t GetSerializedSize (void) const override;
  void Serialize (TagBuffer i) const override;
  void Deserialize (TagBuffer i) override;
  void Print (std::ostream &os) const override;

  void SetPrio (double p);
  double GetPrio () const;

  // 关键：让 vtable 有稳定落点（out-of-line 析构定义在 .cc）
  ~QuicAppPrioTag () override;

private:
  double m_prio; // 0..1
};

} // namespace ns3

#endif // QUIC_APP_PRIO_TAG_H
