// C++ DWARF-shape fixture for dwarf_map: classes, private members, static data
// members, namespace-scope externs, multiple + nested inheritance, std::array,
// enum class, plus a plain C struct as the control.
//
// Built (from this directory, Arm GNU Toolchain 15.2.Rel1 — the toolchain
// sw/cmake/toolchains/arm-none-eabi.cmake selects):
//
//   arm-none-eabi-g++ -std=c++20 -g3 -O0 -fno-exceptions -fno-rtti \
//       -mcpu=cortex-m4 -mthumb -c cxx_layout.cpp -o cxx_layout.o
//   arm-none-eabi-g++ -mcpu=cortex-m4 -mthumb -nostdlib -nostartfiles \
//       -Wl,-e,0 -o cxx_layout.elf cxx_layout.o
//
// Only the linked ELF is checked in: in the relocatable object every
// DW_OP_addr is a zero placeholder awaiting a relocation, so all statics would
// alias at address 0. -nostdlib keeps the image tiny; `use_all` touches every
// object so nothing is dropped, and avoids float arithmetic (which would pull
// __aeabi_fadd from libgcc) and std::array::operator[] (which would pull
// __glibcxx_assert_fail from libstdc++).

#include <array>
#include <cstdint>

// Control: a plain C struct at global scope. Its DW_TAG_variable carries
// DW_AT_name / DW_AT_type / DW_AT_location directly, the pre-C++ shape.
struct PlainC {
    uint32_t count;
    float    gain;
};
PlainC plain_data = { 1u, 2.0f };

namespace cxx {

enum class Mode : uint8_t { Idle = 0, Run = 1, Fault = 2 };

// A struct: public scalars, a C array, two std::array instantiations, an enum
// class member.
struct Layout {
    uint32_t             seq;
    float                bus;
    uint16_t             raw[4];
    std::array<float, 3> duty;
    Mode                 mode;
};

// A class: every data member private, plus a static data member defined out of
// line below.
class Channel {
  public:
    void            bump();
    static uint32_t instances;

  private:
    uint32_t                updateCount;
    float                   current;
    std::array<uint16_t, 4> adc;
    Mode                    mode;
};

class Base {
  public:
    uint32_t baseSeq;
    float    baseGain;
};

class Base2 {
  public:
    uint16_t aux;
    uint32_t auxSeq;
};

// Two bases, in declaration order.
class Derived : public Base, public Base2 {
  public:
    uint32_t             derivedSeq;
    std::array<float, 3> derivedDuty;
};

// A second level, so base flattening has to recurse.
class Deeper : public Derived {
  public:
    uint32_t deepSeq;
};

Layout  layout_data = { 3u, 4.0f, { 1u, 2u, 3u, 4u }, { 0.1f, 0.2f, 0.3f }, Mode::Run };
Channel channel_data;
Derived derived_data;
Deeper  deeper_data;

// Static data member, defined out of line: the defining DW_TAG_variable carries
// only DW_AT_specification back to the in-class declaration.
uint32_t Channel::instances = 5u;

// Namespace-scope extern: declared here, defined below.
extern uint32_t externCounter;

void Channel::bump()
{
    updateCount = 1u;
    current     = 1.0f;
    adc.data()[0] = 2u;
    mode        = Mode::Run;
    instances   = 3u;
}

}  // namespace cxx

namespace cxx {
uint32_t externCounter = 9u;
}

extern "C" void use_all(void)
{
    cxx::channel_data.bump();
    cxx::layout_data.seq  = 1u;
    cxx::layout_data.bus  = 1.0f;
    cxx::layout_data.raw[1] = 2u;
    cxx::layout_data.duty.data()[0] = 1.0f;
    cxx::layout_data.mode = cxx::Mode::Fault;
    cxx::derived_data.baseSeq = 1u;
    cxx::derived_data.baseGain = 1.0f;
    cxx::derived_data.aux = 1u;
    cxx::derived_data.auxSeq = 1u;
    cxx::derived_data.derivedSeq = 1u;
    cxx::derived_data.derivedDuty.data()[2] = 2.0f;
    cxx::deeper_data.baseSeq = 1u;
    cxx::deeper_data.aux = 1u;
    cxx::deeper_data.deepSeq = 1u;
    cxx::externCounter = 1u;
    plain_data.count = 1u;
    plain_data.gain  = 1.0f;
}
