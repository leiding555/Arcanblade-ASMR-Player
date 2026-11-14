"use strict";

function startOfflineTransferReceiver({ available, setStatus, setReceiving, importPackage }) {
  const scope = new URL("./", location.href);
  const directoryName = encodeURIComponent(`arcanblade-transfer:${scope.pathname}`);
  const tempName = "incoming.zip";

  async function openDirectory(create) {
    const root = await navigator.storage.getDirectory();
    return root.getDirectoryHandle(directoryName, { create });
  }

  async function clearTemp(directory) {
    await directory.removeEntry(tempName).catch(error => {
      if (error.name !== "NotFoundError") throw error;
    });
  }

  const legacy = location.hash.startsWith("#receive-pack=");
  if (!legacy && !location.hash.startsWith("#receive-batch=")) {
    if (available) {
      void navigator.locks.request(directoryName, { ifAvailable: true }, async lock => {
        if (lock) await clearTemp(await openDirectory(false));
      }).catch(() => {});
    }
    return null;
  }
  const params = new URLSearchParams(location.hash.slice(1));
  const nonce = params.get(legacy ? "receive-pack" : "receive-batch");
  const source = params.get("source");
  const opener = window.opener;
  history.replaceState(history.state, "", location.pathname + location.search);

  function localOrigin(value) {
    let url;
    try {
      url = new URL(value);
    } catch {
      return false;
    }
    if (url.origin !== value || url.protocol !== "http:"
        || !/^\d{1,3}(?:\.\d{1,3}){3}$/.test(url.hostname)) return false;
    const [first, second] = url.hostname.split(".").map(Number);
    return first === 10 || first === 127 || (first === 192 && second === 168)
      || (first === 172 && second >= 16 && second <= 31)
      || (first === 169 && second === 254) || (first === 100 && second >= 64 && second <= 127)
      || (first === 198 && (second === 18 || second === 19));
  }

  if (!opener || !/^[a-f0-9]{32}$/.test(nonce) || params.size !== 2 || !localOrigin(source)) {
    setStatus("无法连接到发送离线包的局域网页。请返回该网页重试，或下载 ZIP 后导入。");
    return null;
  }

  const controller = new AbortController();
  const { signal } = controller;
  const idleLimit = 90_000;
  const maxChunk = 512 * 1024;
  let port;
  let timeout;
  let openerCheck;
  let importing = false;
  let showImportDetails = false;
  let total;
  let completed = 0;
  let finished = false;

  function cancel(reason = "已取消接收离线包。") {
    controller.abort(new Error(reason));
    if (!port) finish(false, reason);
  }

  function refreshTimeout() {
    clearTimeout(timeout);
    timeout = setTimeout(() => cancel("接收离线包超时，请保持两个页面打开后重试。"), idleLimit);
  }

  function pageHidden() {
    const message = "离线库页面已关闭。";
    cancel(message);
    finish(false, message);
  }

  function finish(ok, message) {
    if (finished) return;
    finished = true;
    clearTimeout(timeout);
    clearInterval(openerCheck);
    window.removeEventListener("message", connect);
    window.removeEventListener("pagehide", pageHidden);
    port?.postMessage({ type: "result", ok, message, completed });
    port?.close();
    setReceiving(false);
    if (!importing && !showImportDetails) {
      setStatus(total ? `${message}已完成 ${completed}/${total} 部作品。` : message);
    }
  }

  // At most one message can wait while storage is being prepared or a chunk is written.
  function readPort() {
    let packet;
    let deliver;
    port.onmessage = ({ data }) => {
      if (data?.type === "cancel") {
        cancel("已取消接收离线包。");
        port.postMessage({ type: "cancelled" });
        return;
      }
      if (signal.aborted) return;
      if (importing || !data || packet !== undefined) {
        cancel("离线包传输顺序无效，请重试。");
      } else if (deliver) {
        deliver(data);
      } else {
        packet = data;
      }
    };
    port.onmessageerror = () => cancel("无法读取传输数据，请重试。");
    return () => {
      signal.throwIfAborted();
      if (packet !== undefined) {
        const data = packet;
        packet = undefined;
        return Promise.resolve(data);
      }
      return new Promise((resolve, reject) => {
        const abort = () => {
          deliver = undefined;
          reject(signal.reason);
        };
        deliver = data => {
          deliver = undefined;
          signal.removeEventListener("abort", abort);
          resolve(data);
        };
        signal.addEventListener("abort", abort, { once: true });
      });
    };
  }

  async function receivePackage(directory, read, index) {
    let writer;
    let cleaned = false;
    try {
      signal.throwIfAborted();
      const handle = await directory.getFileHandle(tempName, { create: true });
      if (typeof handle.createWritable !== "function") {
        throw new Error("当前浏览器不支持直接接收离线包。请先下载 ZIP 后导入。");
      }
      writer = await handle.createWritable();
      signal.throwIfAborted();
      setStatus(`正在接收第 ${index}/${total} 部作品…`);
      port.postMessage(legacy ? { type: "connected" } : { type: "ready-for-package", index });
      let size = 0;
      let sequence = 0;
      let lastUpdate = -Infinity;
      for (;;) {
        const packet = await read();
        signal.throwIfAborted();
        if (packet.type === "end") {
          if (!size || packet.size !== size) throw new Error("收到的离线包不完整，请重试。");
          break;
        }
        const buffer = packet.buffer;
        if (packet.type !== "chunk" || packet.seq !== sequence || !(buffer instanceof ArrayBuffer)
            || buffer.byteLength < 1 || buffer.byteLength > maxChunk
            || !Number.isSafeInteger(size + buffer.byteLength)) {
          throw new Error("收到的离线包数据无效，请重试。");
        }
        refreshTimeout();
        await writer.write(buffer);
        signal.throwIfAborted();
        size += buffer.byteLength;
        const now = performance.now();
        if (now - lastUpdate >= 300) {
          setStatus(`正在接收第 ${index}/${total} 部作品，已接收 ${(size / 1024 ** 2).toFixed(1)} MiB…`);
          lastUpdate = now;
        }
        port.postMessage({ type: "ack", seq: sequence++ });
      }
      await writer.close();
      writer = undefined;
      const file = await handle.getFile();
      signal.throwIfAborted();
      if (file.size !== size) throw new Error("离线包未完整写入，请重试。");

      clearTimeout(timeout);
      importing = true;
      port.postMessage({ type: "importing" });
      const result = await importPackage(file, signal);
      showImportDetails = result?.ok !== true;
      signal.throwIfAborted();
      if (showImportDetails) throw new Error("离线包未完整导入，请查看离线库中的状态。");
      await clearTemp(directory);
      cleaned = true;
    } finally {
      importing = false;
      if (!cleaned) {
        await writer?.abort().catch(() => {});
        await clearTemp(directory).catch(() => {});
      }
    }
  }

  async function receive() {
    if (!available) throw new Error("当前浏览器无法启用离线存储。请下载 ZIP 后在支持的浏览器中导入。");
    const read = readPort();
    await navigator.locks.request(directoryName, { ifAvailable: true }, async lock => {
      if (!lock) throw new Error("离线库正在接收另一批离线包，请稍后重试。");
      const directory = await openDirectory(true);
      await clearTemp(directory);
      signal.throwIfAborted();
      // Older LAN pages send one ZIP immediately after connecting.
      if (legacy) {
        total = 1;
        await receivePackage(directory, read, 1);
        completed = 1;
        return;
      }
      port.postMessage({ type: "connected" });
      for (;;) {
        const packet = await read();
        signal.throwIfAborted();
        if (packet.type === "complete" && completed === total && packet.total === total) return;
        if (packet.type !== "begin" || !Number.isSafeInteger(packet.total) || packet.total < 1
            || (total && total !== packet.total) || packet.index !== completed + 1 || packet.index > packet.total) {
          throw new Error("作品批次传输顺序无效，请重试。");
        }
        total = packet.total;
        refreshTimeout();
        await receivePackage(directory, read, packet.index);
        ++completed;
        refreshTimeout();
        port.postMessage({ type: "package-result", ok: true, index: completed });
      }
    });
  }

  function connect(event) {
    if (event.source !== opener || event.origin !== source || event.data?.nonce !== nonce
        || event.data?.type !== "arcanblade-pack-connect" || event.ports.length !== 1) return;
    window.removeEventListener("message", connect);
    port = event.ports[0];
    refreshTimeout();
    void receive().then(() => finish(true, "离线包已导入。"), error => {
      const message = signal.aborted ? signal.reason.message
        : error.name === "QuotaExceededError" ? "手机存储空间不足。"
          : error.message || "离线包传输失败，请重试。";
      finish(false, message);
    });
  }

  setReceiving(true);
  setStatus("正在等待局域网页发送离线包…");
  window.addEventListener("message", connect);
  window.addEventListener("pagehide", pageHidden);
  refreshTimeout();
  openerCheck = setInterval(() => {
    if (opener.closed) cancel("发送离线包的页面已关闭，请重试。");
  }, 1000);
  opener.postMessage({ type: "arcanblade-pack-ready", nonce }, source);
  return { cancel };
}
