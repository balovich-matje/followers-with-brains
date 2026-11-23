"""Writes FollowersWithBrains.esp: one start-game-enabled quest that hosts the MCM script."""
import struct
import sys

QUEST_ID = 0x01000800  # first own form (Skyrim.esm is master 00)


def sub(tag, data):
    return tag.encode() + struct.pack("<H", len(data)) + data


def zstr(text):
    return text.encode("latin-1") + b"\0"


def wstr(text):
    raw = text.encode("latin-1")
    return struct.pack("<H", len(raw)) + raw


def record(tag, form_id, data, flags=0):
    return tag.encode() + struct.pack("<IIIIHH", len(data), flags, form_id, 0, 44, 0) + data


def group(label, records):
    body = b"".join(records)
    return b"GRUP" + struct.pack("<I4sIIHH", 24 + len(body), label.encode(), 0, 0, 0, 0) + body


def script(name):
    return wstr(name) + b"\0" + struct.pack("<H", 0)  # status 0, no properties


def vmad():
    data = struct.pack("<hhH", 5, 2, 1) + script("FWB_MCM")
    data += struct.pack("<bH", 2, 0)  # fragment data version 2, no fragments
    data += struct.pack("<H", 1)  # one alias with scripts
    data += struct.pack("<hhI", 0, 0, QUEST_ID)  # alias object: unused, alias id 0, quest
    data += struct.pack("<hhH", 5, 2, 1) + script("SKI_PlayerLoadGameAlias")
    return data


def quest():
    data = sub("EDID", zstr("FWB_MCMQuest"))
    data += sub("VMAD", vmad())
    data += sub("FULL", zstr("Followers With Brains MCM"))
    data += sub("DNAM", bytes.fromhex("110100ff0000000000000000"))  # start game enabled, run once
    data += sub("NEXT", b"")
    data += sub("ANAM", struct.pack("<I", 1))
    data += sub("ALST", struct.pack("<I", 0))
    data += sub("ALID", zstr("PlayerRef"))
    data += sub("FNAM", struct.pack("<I", 0))
    data += sub("ALFR", struct.pack("<I", 0x14))  # PlayerRef
    data += sub("VTCK", struct.pack("<I", 0))
    data += sub("ALED", b"")
    return record("QUST", QUEST_ID, data)


def header():
    data = sub("HEDR", struct.pack("<fII", 1.7, 1, 0x801))
    data += sub("CNAM", zstr("Followers With Brains"))
    data += sub("MAST", zstr("Skyrim.esm")) + sub("DATA", struct.pack("<Q", 0))
    data += sub("INTV", struct.pack("<I", 1))
    return record("TES4", 0, data)


with open(sys.argv[1], "wb") as out:
    out.write(header() + group("QUST", [quest()]))
