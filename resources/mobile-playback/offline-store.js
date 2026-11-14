"use strict";

const offlineStore = (() => {
  const scope = new URL("./", location.href);
  const name = `arcanblade-import:${scope.pathname}`;
  const storageFolder = encodeURIComponent(name);
  const available = window.isSecureContext && "serviceWorker" in navigator
    && typeof navigator.storage?.getDirectory === "function"
    && typeof navigator.locks?.request === "function"
    && typeof indexedDB !== "undefined";
  let database;

  function openDatabase() {
    if (!database) database = new Promise((resolve, reject) => {
      const request = indexedDB.open(name, 1);
      request.onupgradeneeded = () => request.result.createObjectStore("tracks", { keyPath: "id" });
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    }).catch(error => {
      database = null;
      throw error;
    });
    return database;
  }

  async function index(mode, action) {
    const db = await openDatabase();
    return new Promise((resolve, reject) => {
      const transaction = db.transaction("tracks", mode);
      const request = action(transaction.objectStore("tracks"));
      transaction.oncomplete = () => resolve(request.result);
      transaction.onabort = () => reject(transaction.error);
    });
  }

  async function getFile(record) {
    const root = await directory();
    const file = await (await root.getFileHandle(record.file)).getFile();
    if (file.size !== record.size) throw new Error("已保存文件不完整。");
    return file;
  }

  async function directory() {
    const root = await navigator.storage.getDirectory();
    return root.getDirectoryHandle(storageFolder, { create: true });
  }

  async function removeFile(file) {
    const root = await directory();
    await root.removeEntry(file).catch(error => {
      if (error.name !== "NotFoundError") throw error;
    });
  }

  async function remove(record) {
    const current = await index("readonly", store => store.get(record.id));
    if (current?.file !== record.file) return;
    await removeFile(record.file);
    await index("readwrite", store => store.delete(record.id));
  }

  function writeAudio(source, file, size, signal) {
    signal.throwIfAborted();
    return new Promise((resolve, reject) => {
      const worker = new Worker(new URL("offline-download.js", scope));
      const cancel = () => worker.postMessage({ cancel: true });
      const finish = () => {
        signal.removeEventListener("abort", cancel);
        worker.terminate();
      };
      worker.onmessage = ({ data }) => {
        finish();
        if (data.error) reject(new DOMException(data.error, data.name));
        else resolve(data);
      };
      worker.onerror = () => {
        finish();
        reject(new Error("无法写入手机存储。"));
      };
      signal.addEventListener("abort", cancel, { once: true });
      worker.postMessage({ ...source, directory: storageFolder, file, size });
    });
  }

  async function save(record, source, signal) {
    const file = crypto.randomUUID();
    try {
      const previous = await index("readonly", store => store.get(record.id));
      if (previous?.crc === record.crc && previous.size === record.size
          && previous.mime === source.mime) source.previous = await getFile(previous).catch(() => null);
      if (!source.previous) {
        const estimate = await navigator.storage.estimate?.().catch(() => null);
        if (estimate && estimate.quota - estimate.usage < record.size)
          throw new DOMException("手机存储空间不足。", "QuotaExceededError");
      }
      const result = await writeAudio(source, file, record.size, signal);
      signal.throwIfAborted();
      if (result.unchanged) {
        if (JSON.stringify(previous.subtitles) === JSON.stringify(record.subtitles)
            && previous.coverCrc === record.coverCrc && previous.workName === record.workName
            && previous.trackName === record.trackName && previous.order === record.order) return "skipped";
        await index("readwrite", store => store.put({ ...record, mime: previous.mime, file: previous.file }));
        return "updated";
      }
      await getFile({ file, size: record.size });
      signal.throwIfAborted();
      await index("readwrite", store => store.put({ ...record, mime: result.mime, file }));
      if (previous) await removeFile(previous.file).catch(() => {});
      return previous ? "updated" : "added";
    } catch (error) {
      await removeFile(file).catch(() => {});
      throw error;
    }
  }

  return {
    available, getFile,
    remove: record => navigator.locks.request(name, () => remove(record)),
    async importPackage(pack, signal, progress) {
      return navigator.locks.request(name, { signal }, async () => {
        for (const track of pack.tracks) {
          signal.throwIfAborted();
          const subtitles = await pack.readSubtitles(track.subtitles);
          const outcome = await save({ id: `${pack.workId}/${track.id}`, workId: pack.workId, workName: pack.workName,
            trackName: track.name, order: track.order, size: track.entry.size, crc: track.entry.crc,
            subtitles, cover: pack.cover, coverCrc: pack.coverCrc },
          { blob: track.entry.blob, expectedCrc: track.entry.crc, mime: track.mime }, signal);
          progress(outcome);
        }
      });
    },
    list: () => index("readonly", store => store.getAll()),
    async prepare() {
      const current = await navigator.serviceWorker.getRegistration(scope);
      if (current?.scope !== scope.href || !current.active) {
        const registration = await navigator.serviceWorker.register(new URL("offline.js", scope));
        if (!registration.active) await new Promise((resolve, reject) => {
          const worker = registration.installing || registration.waiting;
          if (!worker) {
            reject(new Error("无法安装离线页面。"));
            return;
          }
          const timeout = setTimeout(() => finish(new Error("安装离线页面超时。")), 10000);
          function finish(error) {
            clearTimeout(timeout);
            worker.removeEventListener("statechange", check);
            if (error) reject(error);
            else resolve();
          }
          function check() {
            if (worker.state === "activated") finish();
            if (worker.state === "redundant") finish(new Error("无法安装离线页面。"));
          }
          worker.addEventListener("statechange", check);
          check();
        });
      }
      // IndexedDB and OPFS both need to work in the current browser profile.
      await openDatabase();
      const root = await directory();
      await navigator.locks.request(name, { ifAvailable: true }, async lock => {
        if (!lock) return;
        const known = new Set((await index("readonly", store => store.getAll())).map(record => record.file));
        for await (const entry of root.values()) {
          if (entry.kind === "file" && /^[a-f0-9-]{36}$/.test(entry.name) && !known.has(entry.name)) {
            await removeFile(entry.name);
          }
        }
      });
    }
  };
})();
