#include "components/kvm_usb_bridge/hid_keyboard.h"
#include "components/kvm_usb_bridge/switch_hotkey.h"
#include "components/kvm_usb_bridge/report_buffer.h"
#include "fixtures/nuphy_air75_v3_receiver.h"
#include <cassert>
#include <iostream>
#include <vector>
using namespace esphome::kvm_usb_bridge;

static const uint8_t boot_desc[] = {
  0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,
  0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,
  0x75,0x01,0x95,0x08,0x81,0x02,0x95,0x01,
  0x75,0x08,0x81,0x01,0x95,0x05,0x75,0x01,
  0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,
  0x95,0x01,0x75,0x03,0x91,0x01,0x95,0x06,
  0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,
  0x19,0x00,0x29,0x65,0x81,0x00,0xC0
};
int main() {
  KeyboardDescriptor boot;
  assert(boot.parse(boot_desc, sizeof(boot_desc)));
  uint8_t pressed[] = {2,0,4,5,0,0,0,0};
  assert(boot.update(pressed,sizeof(pressed)));
  auto b = boot_report(boot.state());
  assert(b.modifiers==2 && b.keys[0]==4 && b.keys[1]==5 && b.keys[2]==0);
  assert(boot.led_report(boot.layouts()[0],3)==std::vector<uint8_t>{3});
  assert(!boot.update(pressed,3));
  assert(boot_report(boot.state())==b);  // Short input must not create false releases.
  uint8_t released[8]{};
  assert(boot.update(released,sizeof(released)));
  assert(boot_report(boot.state())==BootReport{});
  std::cout << "PASS boot 6KRO, modifiers, padding, LEDs, truncated input, release\n";

  const uint8_t nkro_desc[] = {
    0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,
    0x19,0xE0,0x29,0xE7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
    0x19,0,0x29,0x8F,0x75,1,0x95,0x90,0x81,2,0xC0
  };
  KeyboardDescriptor nkro;
  assert(nkro.parse(nkro_desc,sizeof(nkro_desc)));
  uint8_t bitmap[19]{};
  bitmap[0]=0x82; // LeftShift + RightGUI.
  for(unsigned key=4;key<=10;key++) bitmap[1+key/8] |= 1u << (key%8);
  assert(nkro.update(bitmap,sizeof(bitmap)));
  b=boot_report(nkro.state());
  assert(b.modifiers==0x82);
  for(auto k:b.keys) assert(k==1);
  bitmap[1+10/8] &= ~(1u<<(10%8));
  assert(nkro.update(bitmap,sizeof(bitmap)));
  b=boot_report(nkro.state());
  for(unsigned i=0;i<6;i++) assert(b.keys[i]==4+i);
  std::cout << "PASS 144-bit NKRO -> 6KRO, seventh-key rollover and recovery\n";

  // Multiple report IDs on one interface, plus an unrelated consumer report.
  const uint8_t ids_desc[] = {
    0x05,7,0x85,1,0x19,0xE0,0x29,0xE7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
    0xA4,0x85,2,0x19,0,0x29,0x65,0x15,0,0x25,0x65,0x75,8,0x95,6,0x81,0,0xB4,
    0x85,3,0x05,0x0C,0x09,1,0x75,8,0x95,1,0x81,2,
  };
  KeyboardDescriptor ids;
  assert(ids.parse(ids_desc,sizeof(ids_desc)));
  uint8_t mods[]={1,1}, keys[]={2,6,0,0,0,0,0}, consumer[]={3,0xE9};
  assert(ids.update(mods,sizeof(mods)) && ids.update(keys,sizeof(keys)));
  assert(!ids.update(consumer,sizeof(consumer)));
  b=boot_report(ids.state());
  assert(b.modifiers==1 && b.keys[0]==6);
  KeyState merged=ids.state(); merged.merge(nkro.state());
  assert(merged.down(6) && merged.down(0xE0) && merged.down(0xE1));
  std::cout << "PASS report IDs, Push/Pop, consumer isolation, interface merge\n";
  ids.clear_state();
  assert(boot_report(ids.state())==BootReport{});

  uint8_t rollover[]={0,0,1,1,1,1,1,1};
  assert(boot.update(pressed,sizeof(pressed)));
  assert(!boot.update(rollover,sizeof(rollover)) && boot.state().down(4));
  KeyboardDescriptor unplugged; // Dropping a slot must remove its entire held state.
  assert(boot_report(unplugged.state())==BootReport{});
  const uint8_t bad[]={0xB4};
  assert(!unplugged.parse(bad,sizeof(bad)));
  const uint8_t short_item[]={0x77,1};
  assert(!unplugged.parse(short_item,sizeof(short_item)));
  const uint8_t huge[]={0x05,7,0x75,32,0x97,0xFF,0xFF,0xFF,0xFF,0x81,2};
  assert(!unplugged.parse(huge,sizeof(huge)));
  std::cout << "PASS input error preservation, disconnect release, malformed descriptors\n";

  KeyboardDescriptor real_boot, real_nkro;
  assert(real_boot.parse(NUPHY_INTERFACE_0,sizeof(NUPHY_INTERFACE_0)));
  assert(real_nkro.parse(NUPHY_INTERFACE_1,sizeof(NUPHY_INTERFACE_1)));
  assert(real_boot.layouts()[0].input_bits==64 && real_nkro.layouts()[0].input_bits==152);
  assert(real_boot.led_report(real_boot.layouts()[0],2)==std::vector<uint8_t>{2});
  assert(real_nkro.led_report(real_nkro.layouts()[0],2)==std::vector<uint8_t>{2});
  uint8_t actual_array[]={1,0,4,0,0,0,0,0};
  uint8_t actual_bitmap[19]{};
  actual_bitmap[0]=2;
  actual_bitmap[1]=(1u<<4)|(1u<<5);
  assert(real_boot.update(actual_array,sizeof(actual_array)));
  assert(real_nkro.update(actual_bitmap,sizeof(actual_bitmap)));
  KeyState real_merged=real_boot.state(); real_merged.merge(real_nkro.state());
  b=boot_report(real_merged);
  assert(b.modifiers==3 && b.keys[0]==4 && b.keys[1]==5 && b.keys[2]==0);
  real_boot.clear_state(); // One interface disappears while the other still holds keys.
  b=boot_report(real_nkro.state());
  assert(b.modifiers==2 && b.keys[0]==4 && b.keys[1]==5);
  real_nkro.clear_state();
  assert(boot_report(real_nkro.state())==BootReport{});
  std::cout << "PASS actual Air75 V3 receiver descriptors: 8/19-byte inputs, LEDs, merge and release\n";

  uint32_t rng=0x4E755068;
  for(unsigned n=0;n<10000;n++) {
    uint8_t desc[128], report[1024]{};
    for(auto &v:desc) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; v=uint8_t(rng); }
    KeyboardDescriptor random;
    if(random.parse(desc,(n%sizeof(desc))+1)) {
      random.update(report,sizeof(report));
      for(const auto &layout:random.layouts()) random.led_report(layout,31);
    }
  }
  std::cout << "PASS 10000 malformed/random descriptor robustness cases\n";

  SwitchHotkey disabled;
  KeyState chord;
  chord.press(0xE0); chord.press(0xE2); chord.press(0x38);
  assert(!disabled.observe(chord, 0));
  assert(disabled.filtered_report(chord).modifiers == 5);
  SwitchHotkey hotkey;
  hotkey.set_enabled(true);
  KeyState ctrl_alt;
  ctrl_alt.press(0xE0); ctrl_alt.press(0xE2);
  assert(!hotkey.observe(ctrl_alt, 0));
  assert(hotkey.filtered_report(ctrl_alt).modifiers == 5);
  assert(hotkey.observe(chord, 1));
  assert(hotkey.filtered_report(chord) == BootReport{});
  assert(!hotkey.observe(chord, 2));  // Both receiver interfaces can repeat the chord.
  uint32_t now = 1;
  for (unsigned step = 0; step < 7; step++) {
    assert(hotkey.report_due(now));
    const auto packet = hotkey.next_report();
    assert(packet.modifiers == ((step == 1 || step == 3) ? 0x10 : 0));
    assert(packet.keys[0] == (step == 5 ? 0x4E : 0));
    for (unsigned i = 1; i < 6; i++) assert(packet.keys[i] == 0);
    hotkey.submitted();
    assert(hotkey.waiting_for_ack() && !hotkey.report_due(now + 20));
    assert(hotkey.acknowledge(now + 1) == (step == 6));
    if (step < 6) assert(!hotkey.report_due(now + 30));
    now += 31;
  }
  assert(!hotkey.active());
  assert(hotkey.filtered_report(chord) == BootReport{});  // Held keys never reach new PC.
  KeyState letter;
  letter.press(4);
  assert(!hotkey.observe(letter, now));
  assert(hotkey.filtered_report(letter) == BootReport{});
  assert(!hotkey.observe(KeyState{}, now + 1));
  assert(hotkey.filtered_report(letter).keys[0] == 4);
  assert(!hotkey.observe(chord, now + 2));  // Debounce also consumes the chord.
  assert(!hotkey.active() && hotkey.filtered_report(chord) == BootReport{});
  hotkey.observe(KeyState{}, now + 3);
  std::cout << "PASS Ctrl+Alt+/ consumed, exact acknowledged KVM macro, held-key suppression and debounce\n";

  for (const auto ctrl : {0xE0, 0xE4}) for (const auto alt : {0xE2, 0xE6}) {
    SwitchHotkey sides;
    sides.set_enabled(true);
    KeyState right_or_left;
    right_or_left.press(ctrl); right_or_left.press(alt); right_or_left.press(0x38);
    assert(sides.observe(right_or_left, 0));
  }
  for (const auto extra : {0xE1, 0xE5, 0xE3, 0xE7}) {
    SwitchHotkey exact;
    exact.set_enabled(true);
    KeyState wrong = chord;
    wrong.press(extra);
    assert(!exact.observe(wrong, 0));
    assert(exact.filtered_report(wrong) == boot_report(wrong));
    assert(!exact.observe(chord, 1)); // Releasing Shift/GUI alone must not trigger.
  }
  SwitchHotkey slash_first;
  slash_first.set_enabled(true);
  KeyState slash_only;
  slash_only.press(0x38);
  assert(!slash_first.observe(slash_only, 0));
  assert(slash_first.observe(chord, 1));
  std::array<uint32_t, 3> order{{0x38, 0xE0, 0xE2}};
  do {
    SwitchHotkey simultaneous;
    simultaneous.set_enabled(true);
    KeyState arriving;
    for (unsigned i = 0; i < 3; i++) {
      arriving.press(order[i]);
      assert(simultaneous.observe(arriving, i) == (i == 2));
    }
  } while (std::next_permutation(order.begin(), order.end()));
  std::cout << "PASS left/right modifiers, Shift/GUI exclusion, all six chord arrival orders\n";

  SwitchHotkey failure;
  failure.set_enabled(true);
  assert(failure.observe(chord, 0xFFFFFF00u));
  failure.submitted();
  assert(!failure.timed_out(0xFFFFFF01u));
  assert(failure.timed_out(uint32_t(0xFFFFFF00u + 2000)));
  failure.abort();
  assert(!failure.report_due(3000));
  assert(failure.filtered_report(chord) == BootReport{});
  failure.observe(KeyState{}, 4000);
  assert(failure.filtered_report(letter).keys[0] == 4);
  assert(failure.observe(chord, 4001));
  failure.observe(KeyState{}, 4002); // Physical keys can be released before the macro completes.
  for (unsigned step = 0; step < 7; step++) {
    failure.submitted();
    failure.acknowledge(4003 + 31 * step);
  }
  failure.observe(KeyState{}, 4300);
  assert(failure.filtered_report(letter).keys[0] == 4);
  std::cout << "PASS timeout/abort without replay, timer wrap, and release before sequence completion\n";

  ReportBuffer transport;
  BootReport a_down;
  a_down.keys[0] = 4;
  assert(transport.push(a_down, 10));
  assert(transport.push(BootReport{}, 11));
  assert(!transport.stale(110));
  assert(transport.stale(111));
  // Simulate a host that stops polling, then wakes: the entire old burst is
  // replaced with a release, never a delayed series of a key presses.
  transport.release(2000);
  assert(transport.size() == 1 && transport.front() == BootReport{});
  transport.pop();
  assert(transport.empty());
  for (unsigned i = 0; i < 1000; i++) {
    assert(transport.push(i % 2 ? BootReport{} : a_down, 3000 + i));
    assert(transport.front() == (i % 2 ? BootReport{} : a_down));
    transport.pop();
  }
  assert(transport.empty());
  for (unsigned i = 0; i < ReportBuffer::CAPACITY; i++) assert(transport.push(a_down, 5000));
  assert(!transport.push(a_down, 5001));
  assert(transport.size() == 1 && transport.front() == BootReport{});
  transport.clear();
  assert(transport.push(a_down, 0xFFFFFFF0u));
  assert(!transport.stale(0x20u));
  assert(transport.stale(0x80u));
  transport.pop();
  // A legitimately held key has no undelivered transition after its press was
  // ACKed; it is not forcibly released just because 100ms elapsed.
  assert(transport.empty() && !transport.stale(0x8000u));
  std::cout << "PASS bounded transition queue, FIFO wrap, release-only overflow, stale-wake discard and held keys\n";

  ReleaseGate gate;
  assert(!gate.waiting());
  gate.arm();
  gate.observe(KeyState{}, false); // Synthetic clearing is not a physical release.
  assert(gate.waiting());
  gate.observe(letter, true);
  assert(gate.waiting());
  gate.observe(KeyState{}, true);
  assert(!gate.waiting());
  assert(report_is_fresh(1000, 950, false, 0));
  assert(!report_is_fresh(1000, 899, false, 0));
  assert(!report_is_fresh(1000, 999, true, 1000));
  assert(report_is_fresh(1000, 1000, true, 1000));
  assert(report_is_fresh(0x20, 0xFFFFFFF0u, true, 0xFFFFFFE0u));
  assert(!report_is_fresh(0x20, 0xFFFFFFF0u, true, 0x10u));
  std::cout << "PASS fresh-release gate, suspend/reset boundary, stale input and millisecond wrap\n";

  {
    // Synthetic protocol fixtures only, never a capture of typed input.
    const uint8_t controls[] = {
      0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x01,0x15,0x00,0x26,0xFF,0x03,
      0x19,0x00,0x2A,0xFF,0x03,0x75,0x10,0x95,0x02,0x81,0x00,0xC0,
      0x05,0x01,0x09,0x80,0xA1,0x01,0x85,0x02,0x15,0x00,0x25,0x01,
      0x19,0x81,0x29,0x83,0x75,0x01,0x95,0x03,0x81,0x02,
      0x75,0x05,0x95,0x01,0x81,0x03,0xC0,
      0x05,0x07,0x85,0x03,0x15,0x00,0x25,0x65,0x19,0x00,0x29,0x65,
      0x75,0x08,0x95,0x06,0x81,0x00,
      // Relative AC Pan is mouse movement, not a Consumer button.
      0x05,0x0C,0x85,0x04,0x0A,0x38,0x02,0x15,0x81,0x25,0x7F,
      0x75,0x08,0x95,0x01,0x81,0x06,
    };
    KeyboardDescriptor parsed, disabled;
    assert(parsed.parse(controls, sizeof(controls), true));
    assert(parsed.has_keyboard() && parsed.has_controls());
    assert(disabled.parse(controls, sizeof(controls)) && !disabled.has_controls());
    const uint8_t volume[] = {1,0xE9,0,0xEA,0};
    const uint8_t up[] = {1,0,0,0,0};
    const uint8_t sleep[] = {2,2};
    const uint8_t wake[] = {2,0};
    assert(parsed.controls_report(volume, sizeof(volume)));
    assert(!parsed.keyboard_report(volume, sizeof(volume)));
    assert(parsed.update(volume, sizeof(volume)));
    assert(parsed.controls().consumer[0] == 0xE9 && parsed.controls().consumer[1] == 0xEA);
    assert(parsed.state().bits == KeyState{}.bits);
    assert(!parsed.update(volume, sizeof(volume)-1));
    assert(parsed.controls().consumer[0] == 0xE9);
    assert(parsed.update(sleep, sizeof(sleep)) && parsed.controls().system == 2);
    assert(parsed.controls().consumer[0] == 0xE9);
    assert(parsed.update(up, sizeof(up)) && parsed.controls().consumer[0] == 0);
    assert(parsed.controls().system == 2);
    assert(parsed.update(wake, sizeof(wake)) && parsed.controls().empty());
    const uint8_t mouse_pan[] = {4,1};
    assert(!parsed.controls_report(mouse_pan, sizeof(mouse_pan)));
    assert(!parsed.update(mouse_pan, sizeof(mouse_pan)));
    ReleaseGate keyboard_gate;
    keyboard_gate.arm();
    assert(parsed.update(up, sizeof(up)));
    keyboard_gate.observe(parsed.state(), parsed.keyboard_report(up, sizeof(up)));
    assert(keyboard_gate.waiting()); // Media release cannot unlock keyboard recovery.
    for (uint8_t key = 0x3A; key <= 0x45; key++) {
      const uint8_t function[] = {3,key,0,0,0,0,0};
      assert(parsed.update(function, sizeof(function)));
      assert(boot_report(parsed.state()).keys[0] == key);
    }
    const uint8_t bitmap[] = {0x05,0x0C,0x09,0x01,0xA1,0x01,0x15,0,0x25,1,
      0x09,0xE2,0x09,0xE9,0x09,0xEA,0x75,1,0x95,3,0x81,2,
      0x75,5,0x95,1,0x81,3,0xC0};
    KeyboardDescriptor bits;
    assert(bits.parse(bitmap, sizeof(bitmap), true) && !bits.has_keyboard());
    uint8_t value = 2;
    assert(bits.update(&value, 1) && bits.controls().consumer[0] == 0xE9);
    value = 0;
    assert(bits.update(&value, 1) && bits.controls().empty());
    const uint8_t invalid_bool[] = {0x05,0x0C,0x09,0xE9,0x15,0,0x25,1,
      0x75,8,0x95,1,0x81,2};
    assert(bits.parse(invalid_bool, sizeof(invalid_bool), true));
    value = 2;
    assert(!bits.update(&value, 1) && bits.controls().empty());
    ControlState state;
    assert(state.add(0xE9) && state.add(0xE9) && state.add(0x70));
    assert(state.consumer[0] == 0x70 && state.consumer[1] == 0xE9 && state.consumer[2] == 0);
    const auto packet = ControlPacket::consumer(state);
    assert(packet.id == 1 && packet.length == 16 && packet.data[0] == 0x70 && packet.data[2] == 0xE9);
    assert(ControlPacket::system(0xFF).data[0] == 7);
    ControlReleaseGate media_gate;
    media_gate.arm();
    media_gate.observe(ControlState{}, false);
    assert(media_gate.waiting());
    media_gate.observe(state, true);
    assert(media_gate.waiting());
    media_gate.observe(ControlState{}, true);
    assert(!media_gate.waiting());
    TimedReportBuffer<ControlPacket> queue;
    assert(queue.push(packet, 1) && queue.push(ControlPacket{}, 2));
    assert(queue.front().data[0] == 0x70);
    queue.pop();
    assert(queue.front().data[0] == 0); // Preserve short knob press/release edges.
    assert(queue.stale(103));
    queue.release(200);
    assert(queue.size() == 1 && queue.front().data[0] == 0);
    parsed.update(volume, sizeof(volume));
    parsed.clear_state();
    assert(parsed.controls().empty());
    ControlState full;
    for (unsigned i = 1; i <= ControlState::CAPACITY; i++) assert(full.add(uint16_t(0x100+i)));
    assert(!full.add(0x200));
    std::cout << "PASS Consumer arrays/bitmaps, System controls, Report-ID isolation, F1-F12, relative-mouse exclusion, media release/aging\n";
  }


  {
    KeyboardDescriptor real_media;
    assert(real_media.parse(NUPHY_INTERFACE_2, sizeof(NUPHY_INTERFACE_2), true));
    assert(!real_media.has_keyboard() && real_media.has_controls());
    const uint8_t up[] = {4,0xE9,0}, down[] = {4,0xEA,0}, mute[] = {4,0xE2,0};
    const uint8_t release[] = {4,0,0}, brightness[] = {4,0x6F,0};
    const uint8_t sleep[] = {3,0x82,0}, sys_release[] = {3,0,0};
    for (auto data : {up,down,mute,brightness}) {
      assert(real_media.controls_report(data, 3) && real_media.update(data, 3));
      assert(real_media.controls().consumer[0] == data[1]);
      assert(real_media.update(release, sizeof(release)) && real_media.controls().empty());
    }
    assert(real_media.update(sleep, sizeof(sleep)) && real_media.controls().system == 2);
    assert(real_media.update(sys_release, sizeof(sys_release)) && real_media.controls().empty());
    const uint8_t mouse[] = {2,0,1,0,0,0};
    assert(!real_media.controls_report(mouse, sizeof(mouse)));
    assert(!real_media.update(mouse, sizeof(mouse)));
    std::cout << "PASS actual Air75 V3 media descriptor: volume/mute/brightness, System selector and mouse isolation\n";
  }


  {
    KeyState extended;
    for (auto usage : {0x68,0x73,0x7F,0x80,0x81,0x91}) {
      extended = KeyState{};
      extended.press(usage);
      assert(boot_report(extended).keys[0] == 0);
      assert(boot_report(extended, 0x91).keys[0] == usage);
      SwitchHotkey passthrough;
      assert(passthrough.filtered_report(extended, 0x91).keys[0] == usage);
    }
    extended.press(0x92);
    assert(boot_report(extended, 0x91).keys[1] == 0);
    std::cout << "PASS optional F13-F24/Keyboard volume range with unchanged six-key report shape\n";
  }

  {
    ControlReleaseGate gate;
    ControlState idle, held;
    assert(held.add(0xE9));
    gate.arm_if_held(idle);
    gate.observe(held, true);
    assert(!gate.waiting()); // First real knob pulse after idle attach is usable.
    gate.arm_if_held(held);
    assert(gate.waiting()); // A genuinely held control still needs release.
    gate.arm_if_held(idle);
    gate.observe(idle, false);
    assert(gate.waiting()); // Repeated recovery/synthetic zero cannot unlock it.
    gate.observe(held, true);
    assert(gate.waiting());
    gate.observe(idle, true);
    assert(!gate.waiting());
    gate.arm_if_held(idle);
    gate.observe(held, true);
    assert(!gate.waiting()); // First new pulse after the release is usable too.
    std::cout << "PASS media idle recovery keeps first pulse, held controls retain physical-release quarantine\n";
  }

}
