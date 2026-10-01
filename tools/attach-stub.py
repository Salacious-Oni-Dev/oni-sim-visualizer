#!/usr/bin/env python3
"""Serve oni-framework-api's bulk routes from a captured offline state, with no game running.

WHY THIS EXISTS. `--attach` reads a live Oxygen Not Included, which makes it the one part
of this program that cannot be checked the way everything else is: there is no game in
CI, the game cannot be launched while another session is driving it, and a bug that only
appears against a running game is a bug found by hand, once, by someone watching a
window. That is exactly the failure `--snapshot` was built to remove from the renderer.

So this stands in for the game. It serves the same routes with the same wire format
out of a state file that `simviz --dump-state` wrote and an element table that
`simviz --dump-elements` printed -- both produced by the OFFLINE path over a real corpus.
`--extcells` adds the fourth route, from a blob `simviz --ext-blob` wrote: the sim's own
bytes, handed back unaltered, so the client decodes exactly what a game would send.
That gives the check its teeth: the two sources are then carrying byte-identical arrays
and the same element table, so

    simviz --corpus C --snapshot offline.png
    simviz --attach  S --snapshot live.png

must produce identical PNGs. Any difference is the attach path, because nothing else
differs.

WHAT IT IS NOT. It is not a second implementation of the format to check the first
against -- both sides were written from the same document and a shared bug would be
invisible to this. What it checks is the CLIENT: the HTTP reader, the JSON reader, the
header and entry-table decode, the pointer arithmetic, and that a decoded frame reaches
every view exactly as a simulated one does. The format itself is checked against the real
server, live, and separately.

usage:
  simviz.exe --corpus C --dump-state state.bin --dump-elements > elements.txt
  python3 tools/attach-stub.py state.bin elements.txt [--port 9788] [--host 0.0.0.0]

The state file may be either --dump-state format: v2, which carries every published array
plus the extension and registry blobs, or the older three-array one. The extension blob a
v2 dump carries is NOT served automatically -- pass --extcells explicitly, so that a stub
started without it still exercises the client's 404 path.

`--extcells` also answers /extcells?published=1, the per-frame route. What that reproduces
is the HANDSHAKE -- a property nobody has subscribed to is in no frame, so the first
request for one is answered with JSON and the bytes arrive on a later request -- because
that is the only part of the route a client can get wrong on its own. It does not
reproduce the SUBSET: serving only the named properties means parsing the blob, which is
the one thing this file never learns.
"""

import argparse
import http.server
import json
import re
import struct
import sys

MAGIC = b"ONIGRID\0"
FORMAT_VERSION = 1
HEADER_BYTES = 32
ENTRY_BYTES = 16

# id, name, bytes per cell. Mirrors DebugBulkRoutes.Surfaces and attach.h's copy. A v2
# --dump-state file carries all eighteen, so all eighteen are servable; a v1 one carries
# three, and the other fifteen are then advertised as unpublished -- a state the real server
# also reports and the client must handle, which is why a v1 dump is still worth keeping
# around as a fixture.
SURFACES = [
    (1, "elementIdx", 2),
    (2, "temperature", 4),
    (3, "mass", 4),
    (4, "radiation", 4),
    (5, "properties", 1),
    (6, "insulation", 1),
    (7, "strengthInfo", 1),
    (8, "diseaseIdx", 1),
    (9, "diseaseCount", 4),
    (10, "backwallElement", 2),
    (11, "backwallMass", 4),
    (12, "backwallTemperature", 4),
    (13, "accumulatedFlow", 4),
    (20, "propertyTextureFlow", 8),
    (21, "propertyTextureLiquid", 4),
    (22, "propertyTextureLiquidData", 4),
    (23, "propertyTextureMaterialData", 4),
    (24, "propertyTextureExposedToSunlight", 1),
]
BY_NAME = {name: (sid, bpc) for sid, name, bpc in SURFACES}


STATE_MAGIC = b"ONIS"
STATE_NAME_BYTES = 48


class State:
    """One --dump-state file, in either of the two formats simviz has written.

    V2 is self-describing -- magic, version, world size, a source string, then a record per
    array carrying its own name, type, arity, stride and cell count, then the extension and
    registry blobs. Nothing here needs to know what arrays exist, which is the point: a dump
    from a later build carrying a new surface arrives with its name attached and is served
    like any other.

    V1 had no magic and no records: int32 width, int32 height, then elementIdx, mass and
    temperature back to back. Still read, because dumps of it exist and because the three
    arrays it carries are the three every reader dereferences. A v1 dump is also the easy
    way to exercise the UNPUBLISHED path -- fifteen of the eighteen surfaces are then
    genuinely absent, which is a state the real server reports too.
    """

    def __init__(self, path):
        with open(path, "rb") as f:
            blob = f.read()
        if len(blob) < 8:
            raise SystemExit("%s is too short to be a --dump-state file" % path)
        self.path = path
        self.source = ""
        self.ext_blob = b""
        self.registry_blob = b""
        self.arrays = {}
        if blob[0:4] == STATE_MAGIC:
            self._read_v2(blob)
        else:
            self._read_v1(blob)
        # The wire format declares bytes per cell per surface, and a dump whose array does
        # not match it would be served as a frame the client decodes into nonsense. Caught
        # here rather than there, because "the picture is wrong" is a much worse error
        # message than this one.
        for name, payload in sorted(self.arrays.items()):
            if name not in BY_NAME:
                continue
            want = self.cells * BY_NAME[name][1]
            if len(payload) != want:
                raise SystemExit(
                    "%s: %s is %d bytes; the wire format says %d bytes per cell over %d "
                    "cells is %d" % (path, name, len(payload), BY_NAME[name][1], self.cells, want)
                )

    def _read_v1(self, blob):
        self.version = 1
        self.width, self.height = struct.unpack_from("<ii", blob, 0)
        self.cells = self.width * self.height
        want = 8 + self.cells * (2 + 4 + 4)
        if len(blob) != want:
            raise SystemExit(
                "%s is %d bytes; a v1 %dx%d dump needs %d"
                % (self.path, len(blob), self.width, self.height, want)
            )
        at = 8
        self.arrays["elementIdx"] = blob[at : at + self.cells * 2]
        at += self.cells * 2
        self.arrays["mass"] = blob[at : at + self.cells * 4]
        at += self.cells * 4
        self.arrays["temperature"] = blob[at : at + self.cells * 4]

    def _read_v2(self, blob):
        (self.version,) = struct.unpack_from("<I", blob, 4)
        if self.version != 2:
            raise SystemExit(
                "%s is state-dump version %d and this stub reads 1 and 2"
                % (self.path, self.version)
            )
        self.width, self.height, self.cells, source_bytes = struct.unpack_from("<iiii", blob, 8)
        at = 24
        self.source = blob[at : at + source_bytes].decode("utf-8", "replace")
        at += source_bytes
        (count,) = struct.unpack_from("<i", blob, at)
        at += 4
        for _ in range(count):
            name = blob[at : at + STATE_NAME_BYTES].split(b"\0", 1)[0].decode("ascii", "replace")
            at += STATE_NAME_BYTES
            _type, arity, stride, cells = struct.unpack_from("<iiii", blob, at)
            at += 16
            length = cells * arity * stride
            if at + length > len(blob):
                raise SystemExit("%s: array %s runs past the end of the file" % (self.path, name))
            self.arrays[name] = blob[at : at + length]
            at += length
        (ext_bytes,) = struct.unpack_from("<i", blob, at)
        at += 4
        self.ext_blob = blob[at : at + ext_bytes]
        at += ext_bytes
        (reg_bytes,) = struct.unpack_from("<i", blob, at)
        at += 4
        self.registry_blob = blob[at : at + reg_bytes]
        at += reg_bytes
        if at != len(blob):
            raise SystemExit(
                "%s has %d bytes after its last record" % (self.path, len(blob) - at)
            )


def read_elements(path):
    """Parse --dump-elements output.

    Its columns are: idx, id(hash), state, phase, packed, then two colour readings. The
    PACKED column is the one taken -- it is the element table's own uint32, which is what
    /elements publishes and what views.h unpacks. The two readings beside it are a human
    aid and deliberately ignored here.
    """
    rows = []
    row_re = re.compile(
        r"^\s*(\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+0x([0-9A-Fa-f]{8})\b"
    )
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = row_re.match(line)
            if not m:
                continue
            rows.append(
                {
                    "idx": int(m.group(1)),
                    "idHash": int(m.group(2)),
                    "simState": int(m.group(3)),
                    "colour": int(m.group(5), 16),
                }
            )
    if not rows:
        raise SystemExit("%s carried no element rows; is it --dump-elements output?" % path)
    for i, r in enumerate(rows):
        if r["idx"] != i:
            raise SystemExit("element rows are not in index order at row %d" % i)
    return rows


def json_number(v):
    return str(int(v))


def elements_json(rows, legacy=False):
    """The route's body. `legacy` reproduces the shape it had BEFORE live attach existed:
    no idHash, no colour, no simState. The client has to refuse that by name rather than
    draw a world in whatever colours a zeroed table produces, and this is how that gate is
    checked without keeping an old DLL around to check it against.
    """
    parts = []
    for r in rows:
        if legacy:
            parts.append(
                "{"
                + ",".join(
                    [
                        '"idx":%s' % json_number(r["idx"]),
                        '"id":"STUB_%d"' % r["idx"],
                        '"tag":"STUB_%d"' % r["idx"],
                        '"state":%s' % json_number(r["simState"] & 3),
                        '"stateRaw":%s' % json_number(r["simState"] & ~8),
                        '"lowTemp":0.0',
                        '"highTemp":0.0',
                        '"specificHeatCapacity":0.0',
                        '"thermalConductivity":0.0',
                    ]
                )
                + "}"
            )
            continue
        parts.append(
            "{"
            + ",".join(
                [
                    '"idx":%s' % json_number(r["idx"]),
                    '"id":"STUB_%d"' % r["idx"],
                    '"idHash":%s' % json_number(r["idHash"]),
                    '"tag":"STUB_%d"' % r["idx"],
                    '"state":%s' % json_number(r["simState"] & 3),
                    '"stateRaw":%s' % json_number(r["simState"]),
                    '"simState":%s' % json_number(r["simState"]),
                    '"colour":%s' % json_number(r["colour"]),
                    '"lowTemp":0.0',
                    '"highTemp":0.0',
                    '"specificHeatCapacity":0.0',
                    '"thermalConductivity":0.0',
                ]
            )
            + "}"
        )
    return (
        '{"count":%d,"stateMask":3,"states":["Vacuum","Gas","Liquid","Solid"],"elements":[%s]}'
        % (len(rows), ",".join(parts))
    )


def world_json(state, frame):
    surfaces = ",".join(
        '{"id":%d,"name":"%s","bytesPerCell":%d,"published":%s}'
        % (sid, name, bpc, "true" if name in state.arrays else "false")
        for sid, name, bpc in SURFACES
    )
    return (
        '{"formatVersion":%d,"width":%d,"height":%d,"cells":%d,"unityFrame":%d,'
        '"worlds":[{"id":0,"name":"stub","offsetX":0,"offsetY":0,"width":%d,"height":%d}],'
        '"surfaces":[%s]}'
        % (
            FORMAT_VERSION,
            state.width,
            state.height,
            state.cells,
            frame,
            state.width,
            state.height,
            surfaces,
        )
    )


def grid_body(state, names, frame):
    """Two passes, exactly as DebugBulkRoutes.TryHandleBinary does: size it, then fill it.

    An array the stub cannot serve gets a zero-length entry rather than being dropped, so
    the entry table still answers for every array the query named. That is the case the
    client has to get right -- a null pointer that ViewAvailable then reports honestly.
    """
    entries = []
    offset = HEADER_BYTES + len(names) * ENTRY_BYTES
    offset = (offset + 3) & ~3
    for name in names:
        sid, bpc = BY_NAME[name]
        payload = state.arrays.get(name)
        if payload is None:
            entries.append((sid, bpc, 0, 0, b""))
            continue
        entries.append((sid, bpc, len(payload), offset, payload))
        offset += len(payload)
        offset = (offset + 3) & ~3

    body = bytearray(offset)
    body[0:8] = MAGIC
    struct.pack_into(
        "<IIIIII", body, 8, FORMAT_VERSION, state.width, state.height, state.cells, frame,
        len(names),
    )
    at = HEADER_BYTES
    for sid, bpc, length, byte_offset, _ in entries:
        struct.pack_into("<IIII", body, at, sid, bpc, length, byte_offset)
        at += ENTRY_BYTES
    for _, _, length, byte_offset, payload in entries:
        if length:
            body[byte_offset : byte_offset + length] = payload
    return bytes(body)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("stub: " + (fmt % args) + "\n")

    def reply(self, status, content_type, body):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        # The real server closes after every response, so the client's reader is written
        # for that and is exercised here the same way.
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def error(self, status, message):
        self.reply(status, "application/json", ('{"error":"%s"}' % message).encode())

    def published_extcells(self, props):
        """`/extcells?published=1` -- the same bytes, off the game's per-frame path.

        WHAT THIS STUB CAN AND CANNOT REPRODUCE, because the difference is the point.

        It CAN reproduce the handshake, which is the only part a client can get wrong on its
        own: a property nobody has subscribed to is in no frame yet, so the first request for
        one is answered with JSON naming what was just subscribed, and the bytes arrive on a
        later request. That is `kPublishCellProperty` being queued, and a client that treated
        the JSON as a failure would never make a second request.

        It CANNOT reproduce the SUBSET. Serving only the named properties means parsing the
        blob, and this file deliberately never learns that format -- a fixture that re-encoded
        the sim's bytes through a second writer would agree with any bug that writer has. So
        the body is the same verbatim blob either way, and a test that wants to prove subsetting
        has to ask a real game.
        """
        wanted = [n for n in props.split(",") if n]
        pending = []
        for name in wanted:
            seen = self.server.published_requests.get(name, 0)
            self.server.published_requests[name] = seen + 1
            if seen < self.server.published_latency:
                pending.append(name)
        if pending:
            body = json.dumps(
                {
                    "subscribed": pending,
                    "retry": "next tick",
                    "tick": self.server.frame,
                    "note": "stub: kPublishCellProperty is queued, so a property subscribed "
                    "during tick N first appears in the table tick N+1 publishes",
                }
            ).encode()
            self.reply(200, "application/json", body)
            return
        self.reply(200, "application/octet-stream", self.server.extcells)

    # ---------------------------------------------------------------- /extevents

    # The one place this stub WRITES a format instead of replaying one, and that is a real
    # weakening worth stating rather than burying.
    #
    # /extcells could be served verbatim because a blob the sim itself produced was sitting in
    # a file: the client decoded the sim's own bytes and this file never learned the layout. An
    # event stream is never serialised by the sim -- not a checkpoint component, not a save
    # section -- so no such file can exist, and there is nothing to replay. Synthesising one
    # here means the offline gate proves the C++ reader parses what THIS writer produces, not
    # what OniFramework's writer produces. Those two agreeing is a live-game test and nothing
    # else can stand in for it.
    #
    # What it does gate, and what nothing else could: the handshake, the padding, the three
    # separate gap counters, and a reader's bounds checks -- which is most of what a client can
    # get wrong on its own.
    EVENT_MAGIC = 0x56494E4F  # "ONIV"
    EVENT_STREAMS = {"sim.message_refused": 16}

    def extevents_blob(self, wanted):
        """Build one ONIV reply: a batch per watched stream per synthetic frame."""
        batches = []
        for name in wanted:
            stride = self.EVENT_STREAMS[name]
            n = self.server.event_records
            records = b""
            for i in range(n):
                # ExtRefusedMessage: {messageId, reason, payloadBytes, expectedBytes}. The
                # reason cycles 1..3 so a reader that hardcoded one arm shows up immediately.
                records += struct.pack(
                    "<iiii", 0x4F4E4900 + i, 1 + (i % 3), 8 + i, 16 if i % 3 == 0 else 0
                )
            batches.append((name, stride, n, self.server.event_dropped, records))

        body = struct.pack(
            "<IIii", self.EVENT_MAGIC, 1, len(batches), self.server.event_lost
        )
        for name, stride, count, dropped, records in batches:
            header = name.encode("ascii")[:47]
            header += b"\0" * (48 - len(header))
            header += struct.pack("<iiiii", self.server.frame, stride, count, dropped,
                                  len(records))
            body += header + records
            # Padded to four bytes, exactly as the route does: a stream's stride is whatever
            # its producer declared, so without this a 3-byte record would leave every
            # following header misaligned.
            pad = (-len(records)) % 4
            body += b"\0" * pad
        return body

    def extevents(self, streams):
        declared = sorted(self.EVENT_STREAMS)
        if not streams:
            # A bare GET subscribes nothing and answers with what is declared. This is the
            # discovery step SIM_ExtEventStreamIndex could never give a client with no name.
            self.reply(
                200,
                "application/json",
                json.dumps(
                    {
                        "streams": declared,
                        "detail": [
                            "%s stride=%d %s"
                            % (n, self.EVENT_STREAMS[n],
                               "collecting" if n in self.server.event_subscribed else "idle")
                            for n in declared
                        ],
                        "buffered": 0,
                        "tick": self.server.frame,
                        "note": "stub: add streams=a,b to subscribe and drain",
                    }
                ).encode(),
            )
            return

        wanted = [n for n in streams.split(",") if n]
        for name in wanted:
            if name not in self.EVENT_STREAMS:
                # All-or-nothing, like the real route: one typo must not half-apply.
                self.reply(
                    200,
                    "application/json",
                    json.dumps(
                        {
                            "error": "this SimDLL declares no event stream named %s" % name,
                            "streams": declared,
                            "note": "nothing was subscribed; a request is all-or-nothing",
                        }
                    ).encode(),
                )
                return

        pending = []
        for name in wanted:
            seen = self.server.event_requests.get(name, 0)
            self.server.event_requests[name] = seen + 1
            if seen < self.server.published_latency:
                pending.append(name)
            else:
                self.server.event_subscribed.add(name)
        if pending:
            self.reply(
                200,
                "application/json",
                json.dumps(
                    {
                        "subscribed": pending,
                        "retry": "in two ticks",
                        "tick": self.server.frame,
                        "note": "stub: kSubscribeEventStream is queued, and tick N+1 is the "
                        "first frame that collects",
                    }
                ).encode(),
            )
            return
        self.reply(200, "application/octet-stream", self.extevents_blob(wanted))

    # ---------------------------------------------------------------- /extelements
    #
    # Three attributes rather than one, and deliberately distinct in every field the client
    # reads: an f32 arity 1, an i32 arity 3, and one with no values at all but a non-zero write
    # count. The third is the important one -- it is the state `writes` exists to distinguish
    # from "nobody pushed it", and a stub that only ever served populated attributes could not
    # exercise it.
    #
    # One element id is deliberately NOT in the element table, because `inTable: false` is a
    # real value the client has to render rather than an error: keys are SimHashes ids so a
    # value can outlive the table it was written for.
    def extelements(self, with_values):
        absent = 1234567
        attrs = [
            {
                "idx": 0, "name": "sim.molecular_mass", "type": 0, "typeName": "f32",
                "arity": 1, "stride": 4, "valueCount": 3, "firstParty": True, "writes": 3,
                "values": [
                    {"idHash": -1528777920, "id": "Oxygen", "inTable": True, "v": [31.9988]},
                    {"idHash": -1046145888, "id": "Hydrogen", "inTable": True, "v": [2.01588]},
                    {"idHash": absent, "id": None, "inTable": False, "v": [44.01]},
                ],
            },
            {
                "idx": 1, "name": "mod9.curve", "type": 3, "typeName": "i32",
                "arity": 3, "stride": 4, "valueCount": 1, "firstParty": False, "writes": 4,
                "values": [
                    {"idHash": -1528777920, "id": "Oxygen", "inTable": True,
                     "v": [11, -22, 33]},
                ],
            },
            {
                "idx": 2, "name": "mod9.cleared", "type": 0, "typeName": "f32",
                "arity": 1, "stride": 4, "valueCount": 0, "firstParty": False, "writes": 7,
                "values": [],
            },
        ]
        if not with_values:
            for a in attrs:
                del a["values"]
        body = json.dumps({
            "count": len(attrs),
            "withValues": with_values,
            "attributes": attrs,
            "note": "keys are SimHashes ids, never element-table indices -- inTable false "
                    "is a real value for an element the loaded table has no row for",
        }).encode()
        self.reply(200, "application/json", body)

    def do_GET(self):
        state = self.server.state
        # Unity's frame counter moves on every request in the real server, and a client
        # that compared whole bodies to decide whether the sim had stepped would be fooled
        # by exactly that. Reproduced here so the pitfall is reproducible too.
        self.server.frame += 1
        path, _, query = self.path.partition("?")
        if path == "/world":
            self.reply(200, "application/json", world_json(state, self.server.frame).encode())
        elif path == "/elements":
            self.reply(200, "application/json", self.server.elements_body)
        elif path == "/grid":
            arrays = ""
            for part in query.split("&"):
                key, _, value = part.partition("=")
                if key == "arrays":
                    arrays = value
            if not arrays:
                self.error(400, "/grid wants ?arrays=name,name or ?arrays=all")
                return
            if arrays == "all":
                names = [name for _, name, _ in SURFACES]
            else:
                names = [n for n in arrays.split(",") if n]
            for n in names:
                if n not in BY_NAME:
                    self.error(400, "unknown array %s" % n)
                    return
            body = grid_body(state, names, self.server.frame)
            if self.server.corrupt:
                # Half a body, with the header and entry table intact. The entries then
                # point past the end, which is the one malformed reply that would reach a
                # renderer as a wild pointer if the client validated lazily. It must be
                # refused with the previous frame left standing.
                body = body[: len(body) // 2]
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)
                self.close_connection = True
                return
            self.reply(200, "application/octet-stream", body)
        elif path == "/extcells":
            # Served verbatim, the way the real route does: the body is exactly what
            # SIM_DebugExtCellStateAll produced, so the client decodes the sim's own bytes
            # and this stub never learns the format. Without --extcells the route is
            # missing, which is what a client sees against an OniFramework that predates it.
            #
            # ?list=1 is the real server's human-readable summary and is not reproduced --
            # answering it would mean parsing the blob here, which is the one thing this
            # file is careful not to do.
            params = {}
            for part in query.split("&"):
                key, _, value = part.partition("=")
                params[key] = value
            if self.server.extcells is None:
                self.error(404, "no route %s" % path)
            elif "published" in params:
                self.published_extcells(params.get("props", ""))
            elif "list" in params:
                self.error(400, "the stub serves /extcells bytes only, not ?list=1")
            else:
                self.reply(200, "application/octet-stream", self.server.extcells)
        elif path == "/extelements":
            # SYNTHESISED, like /extevents and unlike /extcells. There is no blob of the sim's
            # own bytes to hand back here for a reason worth stating: registry 2 is never
            # serialised at all -- nothing in it is saved, and it is not on the published frame
            # -- so the only file that could exist would be one this stub wrote. What this gates
            # is therefore the JSON READER against this writer, not against OniFramework's. Only
            # a live game gates the two writers against each other, as with the ONIV event
            # stream.
            if not self.server.elementattrs:
                self.error(404, "no route %s" % path)
            else:
                params = {}
                for part in query.split("&"):
                    key, _, value = part.partition("=")
                    params[key] = value
                self.extelements(params.get("values", "1") != "0")
        elif path == "/extevents":
            if not self.server.events:
                self.error(404, "no route %s" % path)
            else:
                params = {}
                for part in query.split("&"):
                    key, _, value = part.partition("=")
                    params[key] = value
                self.extevents(params.get("streams", ""))
        else:
            self.error(404, "no route %s" % path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("state", help="a file written by simviz --dump-state")
    ap.add_argument("elements", help="a file holding simviz --dump-elements output")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=9788)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument(
        "--legacy-elements",
        action="store_true",
        help="serve /elements without idHash/colour/simState, as it was before attach",
    )
    ap.add_argument(
        "--corrupt",
        action="store_true",
        help="truncate every /grid body to half its length, entry table intact",
    )
    ap.add_argument(
        "--published-latency",
        type=int,
        default=1,
        help="how many times /extcells?published=1 answers 'subscribed, retry' for a "
        "property before serving it (default 1, the real one-tick queue latency; 0 serves "
        "immediately, which is what a property another mod already publishes looks like)",
    )
    ap.add_argument(
        "--events",
        action="store_true",
        help="serve /extevents. SYNTHESISED, not replayed: an event stream is never "
        "serialised by the sim, so unlike --extcells there is no file of the sim's own "
        "bytes to hand back, and this writes the wire format itself. That gates the "
        "reader against THIS writer, not against OniFramework's -- only a live game gates "
        "the two writers against each other",
    )
    ap.add_argument(
        "--event-records",
        type=int,
        default=3,
        help="records per batch (default 3; 0 is a batch with none, which is what a frame "
        "that only dropped looks like)",
    )
    ap.add_argument(
        "--event-dropped",
        type=int,
        default=0,
        help="the per-batch `dropped` field: records the SIM refused at its own per-frame cap",
    )
    ap.add_argument(
        "--event-lost",
        type=int,
        default=0,
        help="the reply's `lost` field: whole batches the ROUTE dropped from its ring. Kept "
        "separate from --event-dropped on purpose -- the two are different failures",
    )
    ap.add_argument(
        "--element-attributes",
        action="store_true",
        help="serve /extelements, registry 2. SYNTHESISED for the reason --events is: this "
        "registry is never serialised, so there is no file of the sim's own bytes to replay. "
        "Three attributes, one of them with 0 values and a non-zero write count, and one key "
        "that is not in the element table",
    )
    ap.add_argument(
        "--extcells",
        metavar="FILE",
        help="serve this file, verbatim, as /extcells -- write one with "
        "simviz --ext-blob. Without it the route is missing, as on an older OniFramework",
    )
    args = ap.parse_args()

    state = State(args.state)
    rows = read_elements(args.elements)
    server = http.server.ThreadingHTTPServer((args.host, args.port), Handler)
    server.state = state
    server.elements_body = elements_json(rows, args.legacy_elements).encode()
    server.frame = 0
    server.verbose = args.verbose
    server.corrupt = args.corrupt
    server.extcells = None
    server.published_requests = {}
    server.published_latency = args.published_latency
    server.events = args.events
    server.elementattrs = args.element_attributes
    server.event_requests = {}
    server.event_subscribed = set()
    server.event_records = args.event_records
    server.event_dropped = args.event_dropped
    server.event_lost = args.event_lost
    if args.extcells:
        with open(args.extcells, "rb") as f:
            server.extcells = f.read()
    print(
        "stub serving %dx%d (%d cells), %d elements, %d of %d surfaces published, on %s:%d"
        % (
            state.width,
            state.height,
            state.cells,
            len(rows),
            len(state.arrays),
            len(SURFACES),
            args.host,
            args.port,
        ),
        flush=True,
    )
    if args.legacy_elements:
        print("  /elements is in its PRE-ATTACH shape; the client should refuse", flush=True)
    if args.corrupt:
        print("  /grid bodies are TRUNCATED; the client should refuse each poll", flush=True)
    if server.extcells is not None:
        print("  /extcells serves %d bytes from %s" % (len(server.extcells), args.extcells),
              flush=True)
    if server.elementattrs:
        print("  /extelements serves 3 synthesised attributes", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
