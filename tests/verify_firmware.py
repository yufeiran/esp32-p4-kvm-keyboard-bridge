"""Audit the actual USB descriptors in the linked P4 ELF (requires pyelftools)."""
import argparse
import hashlib
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile

parser = argparse.ArgumentParser()
parser.add_argument('elf', type=Path)
parser.add_argument('--bin', type=Path)
args = parser.parse_args()
descriptors = {}
with args.elf.open('rb') as stream:
    elf = ELFFile(stream)
    for symbol in elf.get_section_by_name('.symtab').iter_symbols():
        if 'kvm_usb_bridge' not in symbol.name:
            continue
        for name in ('DEVICE', 'CONFIG', 'REPORT'):
            if f'{name}_DESCRIPTOR' in symbol.name:
                section = elf.get_section(symbol['st_shndx'])
                offset = symbol['st_value'] - section['sh_addr']
                descriptors[name] = section.data()[offset:offset + symbol['st_size']]

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
if args.bin:
    print('OTA SHA256', hashlib.sha256(args.bin.read_bytes()).hexdigest())
