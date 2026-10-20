"""Audit the actual USB descriptors in the linked P4 ELF (requires pyelftools)."""
import argparse
import hashlib
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile

parser = argparse.ArgumentParser()
parser.add_argument('elf', type=Path)
parser.add_argument('--bin', type=Path)
parser.add_argument('--hub', action='store_true', help='Require the optional hub driver, compiled HCD ABI and FS/LS reset wrapper')
args = parser.parse_args()
descriptors = {}
linked_symbols = set()
hcd_reset_abi = None
with args.elf.open('rb') as stream:
    elf = ELFFile(stream)
    for symbol in elf.get_section_by_name('.symtab').iter_symbols():
        if symbol['st_shndx'] != 'SHN_UNDEF' and symbol['st_value']:
            linked_symbols.add(symbol.name)
        if 'kvm_usb_bridge' not in symbol.name:
            continue
        for name in ('DEVICE', 'CONFIG', 'REPORT'):
            if f'{name}_DESCRIPTOR' in symbol.name:
                section = elf.get_section(symbol['st_shndx'])
                offset = symbol['st_value'] - section['sh_addr']
                descriptors[name] = section.data()[offset:offset + symbol['st_size']]
    if args.hub and elf.has_dwarf_info():
        # The compatibility shim uses a private HCD ABI. Check the actual
        # driver's enum in DWARF, not a duplicate declaration in the shim.
        for cu in elf.get_dwarf_info().iter_CUs():
            source = cu.get_top_DIE().attributes.get('DW_AT_name')
            if source is None or not source.value.endswith(b'hcd_dwc.c'):
                continue
            for die in cu.iter_DIEs():
                name = die.attributes.get('DW_AT_name')
                if die.tag != 'DW_TAG_typedef' or name is None or name.value != b'hcd_port_cmd_t':
                    continue
                enum = die.get_DIE_from_attribute('DW_AT_type')
                values = {child.attributes['DW_AT_name'].value: child.attributes['DW_AT_const_value'].value
                          for child in enum.iter_children() if child.tag == 'DW_TAG_enumerator'}
                hcd_reset_abi = (enum.attributes['DW_AT_byte_size'].value, values.get(b'HCD_PORT_CMD_RESET'))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)
    print('PASS', message)

device, config, report = (descriptors[name] for name in ('DEVICE', 'CONFIG', 'REPORT'))
require(len(device) == 18 and device[:4] == bytes.fromhex('12 01 10 01'), 'USB 1.10 device descriptor')
require(device[4:7] == b'\0\0\0' and device[7] == 64 and device[17] == 1,
        'class per interface, EP0 64 bytes, one configuration')
require(len(config) == 34 and struct.unpack_from('<H', config, 2)[0] == 34 and config[4] == 1,
        'one interface, complete 34-byte configuration descriptor')
require(config[9:18] == bytes.fromhex('09 04 00 00 01 03 01 01 00'),
        'HID Boot Keyboard interface, one endpoint')
require(config[18:27] == bytes.fromhex('09 21 11 01 00 01 22 3f 00') and len(report) == 63,
        'HID 1.11, correct 63-byte report descriptor length')
require(config[27:] == bytes.fromhex('07 05 81 03 08 00 01'),
        'interrupt IN 0x81, eight-byte maximum packet, one-millisecond interval')

# Walk HID short/long items, so an operand byte 0x85 is not mistaken for a Report ID.
pos = 0
while pos < len(report):
    prefix = report[pos]
    pos += 1
    if prefix == 0xFE:
        size = report[pos]
        pos += size + 2
        continue
    size = prefix & 3
    size = 4 if size == 3 else size
    if prefix == 0x85:
        raise RuntimeError('Output descriptor contains a Report ID')
    pos += size
require(pos == len(report), 'well-bounded HID items and no Report ID')
wrappers = {'__wrap_hcd_port_command'}
if args.hub:
    require(hcd_reset_abi == (4, 2), 'compiled HCD ABI: 32-bit command enum, RESET=2')
    require(wrappers <= linked_symbols, 'FS/LS-only root-reset wrapper survives link GC')
    require({'hcd_port_command', 'ext_hub_install'} <= linked_symbols,
            'original HCD command implementation and external hub driver remain linked')
else:
    require(not wrappers & linked_symbols, 'direct profile does not link the hub-only reset wrapper')
if args.bin:
    print('OTA SHA256', hashlib.sha256(args.bin.read_bytes()).hexdigest())
