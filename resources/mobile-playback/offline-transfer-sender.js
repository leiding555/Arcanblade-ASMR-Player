"use strict";

// Wait for the offline page to accept a private port before requesting any media.
async function exportOfflinePackages({ receiverUrl, packages, signal, setStatus }) {
  const totalCount = packages.length;
  if (!totalCount) throw new Error("请选择要导出的作品。");
  signal.throwIfAborted();
  const nonce = Array.from(crypto.getRandomValues(new Uint8Array(16)), byte =>
    byte.toString(16).padStart(2, "0")).join("");
  const destination = new URL(receiverUrl);
  destination.hash = new URLSearchParams({ "receive-batch": nonce, source: location.origin }).toString();
  const receiver = window.open(destination.href, "_blank");
  if (!receiver) throw new Error("浏览器阻止了打开离线库，请允许打开新页面后重试。");

  const { port1, port2 } = new MessageChannel();
  const transfer = new AbortController();
  let pending;
  let cancelAcknowledged;
  let connected = false;
  let receiverFinished = false;
  let completedCount = 0;
  const cancel = () => transfer.abort(signal.reason);
  const pageHidden = () => {
    if (connected) port1.postMessage({ type: "cancel" });
    transfer.abort(new Error("发送离线包的页面已关闭。"));
  };
  signal.addEventListener("abort", cancel, { once: true });
  window.addEventListener("pagehide", pageHidden);
  if (signal.aborted) cancel();

  function waitFor(type, send, timeout = 90000) {
    transfer.signal.throwIfAborted();
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => stop(new Error("离线库长时间未响应，请重试。")), timeout);
      const abort = () => stop(transfer.signal.reason);
      function stop(error, data) {
        clearTimeout(timer);
        transfer.signal.removeEventListener("abort", abort);
        pending = null;
        if (error) reject(error);
        else resolve(data);
      }
      pending = { type, stop };
      transfer.signal.addEventListener("abort", abort, { once: true });
      try { send?.(); }
      catch (error) { stop(error); }
    });
  }

  const onReady = event => {
    if (event.source !== receiver || event.origin !== destination.origin
        || event.data?.type !== "arcanblade-pack-ready" || event.data.nonce !== nonce) return;
    if (pending?.type === "ready") pending.stop();
  };
  window.addEventListener("message", onReady);
  port1.onmessage = ({ data }) => {
    if (data?.type === "result") receiverFinished = true;
    if (data?.type === "cancelled" || data?.type === "result") cancelAcknowledged?.();
    if (data?.type === "result" && data.ok === false) {
      transfer.abort(new Error(typeof data.message === "string" ? data.message : "离线库未完成导入。"));
    } else if (pending && pending.type === data?.type) {
      pending.stop(null, data);
    }
  };
  port1.onmessageerror = () => transfer.abort(new Error("无法读取离线库的传输消息。"));
  const closedCheck = setInterval(() => {
    if (receiver.closed) transfer.abort(new Error("与离线库的页面连接已关闭。"));
  }, 1000);

  async function cancelReceiver() {
    if (!connected || receiverFinished || receiver.closed) return;
    await new Promise(resolve => {
      const timer = setTimeout(finish, 1000);
      function finish() {
        clearTimeout(timer);
        cancelAcknowledged = undefined;
        resolve();
      }
      cancelAcknowledged = finish;
      port1.postMessage({ type: "cancel" });
    });
  }

  async function sendPackage({ name, packageUrl, selection }, index) {
    const progress = `${index}/${totalCount} · ${name}`;
    setStatus(`正在准备导出 ${progress}…`);
    const begin = await waitFor("ready-for-package", () => {
      port1.postMessage({ type: "begin", index, total: totalCount });
    });
    if (begin.index !== index) throw new Error("离线包传输顺序异常。");
    setStatus(`正在从电脑读取离线包 ${progress}…`);
    const response = await fetch(packageUrl, {
      method: "POST", body: new URLSearchParams({ tracks: selection }),
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      credentials: "same-origin", redirect: "error", signal: transfer.signal
    });
    if (!response.ok) {
      const problem = await response.json().catch(() => ({}));
      throw new Error(problem.error || `电脑无法生成离线包（${response.status}）。`);
    }
    if (!response.body || response.headers.get("content-type")?.split(";")[0] !== "application/zip") {
      throw new Error("电脑未返回有效的离线包响应。");
    }
    const reader = response.body.getReader();
    let size = 0;
    let seq = 0;
    let lastStatus = 0;
    const chunkSize = 512 * 1024;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        for (let offset = 0; offset < value.length; offset += chunkSize) {
          const buffer = value.slice(offset, offset + chunkSize).buffer;
          const length = buffer.byteLength;
          const acknowledgement = await waitFor("ack", () => port1.postMessage({ type: "chunk", seq, buffer }, [buffer]));
          if (acknowledgement.seq !== seq++) throw new Error("离线包传输顺序异常。");
          size += length;
        }
        const now = performance.now();
        if (now - lastStatus >= 300) {
          setStatus(`正在传输离线包 ${progress} · 已传输 ${(size / 1024 ** 2).toFixed(1)} MB…`);
          lastStatus = now;
        }
      }
    } finally {
      reader.releaseLock();
    }
    await waitFor("importing", () => port1.postMessage({ type: "end", size }));
    setStatus(`离线库正在校验并导入 ${progress}…`);
    const result = await waitFor("package-result", null, 60 * 60 * 1000);
    if (result.index !== index || result.ok !== true) throw new Error("离线库未确认导入结果。");
  }

  try {
    setStatus(`正在连接离线库，将导出 ${totalCount} 部作品…`);
    await waitFor("ready", null, 25000);
    window.removeEventListener("message", onReady);
    await waitFor("connected", () => {
      receiver.postMessage({ type: "arcanblade-pack-connect", nonce }, destination.origin, [port2]);
      connected = true;
    });
    for (const item of packages) {
      await sendPackage(item, completedCount + 1);
      ++completedCount;
    }
    const result = await waitFor("result", () => port1.postMessage({ type: "complete", total: totalCount }));
    if (result.ok !== true || result.completed !== completedCount) throw new Error("离线库未确认导入结果。");
    return { completedCount };
  } catch (error) {
    transfer.abort(error);
    await cancelReceiver();
    const failure = new Error(error.message, { cause: error });
    failure.completedCount = completedCount;
    failure.packageName = packages[completedCount]?.name;
    throw failure;
  } finally {
    transfer.abort();
    signal.removeEventListener("abort", cancel);
    window.removeEventListener("pagehide", pageHidden);
    window.removeEventListener("message", onReady);
    clearInterval(closedCheck);
    port1.close();
    port2.close();
  }
}
