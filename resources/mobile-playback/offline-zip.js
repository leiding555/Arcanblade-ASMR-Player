"use strict";

// The desktop exports stored ZIP entries. Read only their headers and the requested byte ranges.
const offlineZip = (() => {
  const invalid = () => new Error("离线包格式无效、损坏或尚未下载完成。");
  const view = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const limit = 0xffffffff;
  const maxEntries = 8194;
  const crcTable = Uint32Array.from({ length: 256 }, (_, index) => {
    for (let bit = 0; bit < 8; bit++) index = (index >>> 1) ^ ((index & 1) ? 0xedb88320 : 0);
    return index;
  });

  function updateCrc(crc, bytes) {
    for (const byte of bytes) crc = (crc >>> 8) ^ crcTable[(crc ^ byte) & 255];
    return crc >>> 0;
  }

  function large(data, offset) {
    const value = Number(data.getBigUint64(offset, true));
    if (!Number.isSafeInteger(value)) throw invalid();
    return value;
  }

  async function open(file) {
    const read = async (offset, length) => {
      if (offset < 0 || length < 0 || offset > file.size - length) throw invalid();
      return new Uint8Array(await file.slice(offset, offset + length).arrayBuffer());
    };
    const end = view(await read(file.size - 22, 22));
    if (end.getUint32(0, true) !== 0x06054b50 || end.getUint32(4, true) || end.getUint16(20, true)) throw invalid();
    let count = end.getUint16(10, true);
    if (count !== end.getUint16(8, true)) throw invalid();
    let size = end.getUint32(12, true), offset = end.getUint32(16, true);
    let directoryEnd = file.size - 22;
    if (count === 65535 || size === limit || offset === limit) {
      const locator = view(await read(directoryEnd - 20, 20));
      directoryEnd -= 76;
      if (locator.getUint32(0, true) !== 0x07064b50 || locator.getUint32(4, true)
          || locator.getUint32(16, true) !== 1 || large(locator, 8) !== directoryEnd) throw invalid();
      const zip64 = view(await read(directoryEnd, 56));
      if (zip64.getUint32(0, true) !== 0x06064b50 || large(zip64, 4) !== 44
          || large(zip64, 16) || large(zip64, 24) !== large(zip64, 32)) throw invalid();
      count = large(zip64, 32); size = large(zip64, 40); offset = large(zip64, 48);
    }
    if (count < 4 || count > maxEntries || size > 2 * 1024 ** 2 || offset !== directoryEnd - size) throw invalid();
    const directory = await read(offset, size);
    const data = view(directory);
    const decoder = new TextDecoder("utf-8", { fatal: true });
    const entries = new Map();
    let pos = 0, expectedLocal = 0;
    for (let i = 0; i < count; i++) {
      if (pos > size - 46 || data.getUint32(pos, true) !== 0x02014b50
          || data.getUint16(pos + 8, true) !== 8 || data.getUint16(pos + 10, true)
          || data.getUint16(pos + 32, true) || data.getUint16(pos + 34, true)) throw invalid();
      const nameSize = data.getUint16(pos + 28, true), extraSize = data.getUint16(pos + 30, true);
      const extra = pos + 46 + nameSize, next = extra + extraSize;
      if (next > size || nameSize > 64) throw invalid();
      const name = decoder.decode(directory.subarray(pos + 46, extra));
      if (!/^(manifest\.json|cover\.png|audio-[1-9]\d{0,18}\.(mp3|wav)|subtitles-[1-9]\d{0,18}\.json)$/.test(name)
          || entries.has(name)) throw invalid();
      let bytes = data.getUint32(pos + 24, true), local = data.getUint32(pos + 42, true);
      const crc = data.getUint32(pos + 16, true);
      if (data.getUint32(pos + 20, true) !== bytes) throw invalid();
      const size64 = bytes === limit;
      const length64 = (size64 ? 16 : 0) + (local === limit ? 8 : 0);
      if (extraSize !== (length64 ? length64 + 4 : 0)) throw invalid();
      if (length64) {
        if (data.getUint16(extra, true) !== 1 || data.getUint16(extra + 2, true) !== length64) throw invalid();
        if (size64) {
          bytes = large(data, extra + 4);
          if (bytes < limit || bytes !== large(data, extra + 12)) throw invalid();
        }
        if (local === limit) local = large(data, extra + 4 + (size64 ? 16 : 0));
      }
      if (local !== expectedLocal || local > offset - 30) throw invalid();
      const header = view(await read(local, 30));
      const localExtra = header.getUint16(28, true);
      if (header.getUint32(0, true) !== 0x04034b50 || header.getUint16(6, true) !== 8
          || header.getUint16(8, true) || header.getUint16(26, true) !== nameSize
          || header.getUint32(14, true) || header.getUint32(18, true) !== (size64 ? limit : 0)
          || header.getUint32(22, true) !== (size64 ? limit : 0)
          || localExtra !== (size64 ? 20 : 0)
          || decoder.decode(await read(local + 30, nameSize)) !== name) throw invalid();
      if (size64) {
        const extra = view(await read(local + 30 + nameSize, localExtra));
        if (extra.getUint16(0, true) !== 1 || extra.getUint16(2, true) !== 16
            || large(extra, 4) !== bytes || large(extra, 12) !== bytes) throw invalid();
      }
      const start = local + 30 + nameSize + localExtra;
      const footerSize = size64 ? 24 : 16;
      if (bytes > offset - start - footerSize) throw invalid();
      const footer = view(await read(start + bytes, footerSize));
      if (footer.getUint32(0, true) !== 0x08074b50 || footer.getUint32(4, true) !== crc
          || (size64 ? large(footer, 8) !== bytes || large(footer, 16) !== bytes
            : footer.getUint32(8, true) !== bytes || footer.getUint32(12, true) !== bytes)) throw invalid();
      entries.set(name, { size: bytes, crc, blob: file.slice(start, start + bytes) });
      expectedLocal = start + bytes + footerSize;
      pos = next;
    }
    if (pos !== size || expectedLocal !== offset) throw invalid();
    return entries;
  }

  async function metadata(entry, maxBytes) {
    if (!entry || entry.size <= 0 || entry.size > maxBytes) throw invalid();
    const bytes = new Uint8Array(await entry.blob.arrayBuffer());
    if (((updateCrc(0xffffffff, bytes) ^ 0xffffffff) >>> 0) !== entry.crc) throw invalid();
    return bytes;
  }

  async function inspect(file) {
    const entries = await open(file);
    const decode = async (entry, size) => JSON.parse(new TextDecoder("utf-8", { fatal: true })
      .decode(await metadata(entry, size)));
    const info = await decode(entries.get("manifest.json"), 8 * 1024 ** 2);
    if (info?.format !== "arcanblade-offline" || info.version !== 1) throw new Error("此离线包版本不受支持。");
    if (typeof info.work_id !== "string" || !/^[a-f0-9]{64}$/.test(info.work_id)
        || typeof info.work_name !== "string" || !info.work_name || !Array.isArray(info.tracks)
        || !info.tracks.length || entries.size !== 2 + 2 * info.tracks.length) throw invalid();
    const ids = new Set();
    const tracks = info.tracks.map(track => {
      if (typeof track?.id !== "string" || !/^[1-9]\d{0,18}$/.test(track.id) || ids.has(track.id)
          || !Number.isSafeInteger(track.order) || track.order < 0 || typeof track.name !== "string" || !track.name
          || (track.audio !== `audio-${track.id}.wav` && track.audio !== `audio-${track.id}.mp3`)
          || track.subtitles !== `subtitles-${track.id}.json`) throw invalid();
      ids.add(track.id);
      const entry = entries.get(track.audio), subtitles = entries.get(track.subtitles);
      if (!entry?.size || !subtitles?.size || subtitles.size > 64 * 1024 ** 2) throw invalid();
      return { ...track, entry, subtitles, mime: track.audio.endsWith(".wav") ? "audio/wav" : "audio/mpeg" };
    });
    const cover = entries.get("cover.png");
    const image = await metadata(cover, 16 * 1024 ** 2);
    if (image.length < 8 || view(image).getBigUint64(0) !== 0x89504e470d0a1a0an) throw invalid();
    return { workId: info.work_id, workName: info.work_name, cover: new Blob([image], { type: "image/png" }),
      coverCrc: cover.crc, tracks, async readSubtitles(entry) {
        const payload = await decode(entry, 64 * 1024 ** 2);
        if (!payload || !["none", "disabled", "unavailable", "ready"].includes(payload.state)
            || !Array.isArray(payload.entries) || (payload.state !== "ready" && payload.entries.length)
            || payload.entries.some(cue => !Array.isArray(cue) || cue.length !== 3
              || !Number.isSafeInteger(cue[0]) || !Number.isSafeInteger(cue[1]) || cue[0] < 0
              || cue[1] < cue[0] || typeof cue[2] !== "string")) throw invalid();
        return payload;
      } };
  }

  return { inspect, updateCrc };
})();
