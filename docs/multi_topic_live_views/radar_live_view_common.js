(function (global) {
  "use strict";

  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  const enc = encodeURIComponent;
  const finite = value => Number.isFinite(Number(value));
  const n = (value, fallback = NaN) => finite(value) ? Number(value) : fallback;
  const fmt = (value, digits = 1, fallback = "—") => finite(value) ? Number(value).toFixed(digits) : fallback;
  const integer = (value, fallback = "—") => finite(value) ? Math.round(Number(value)).toString() : fallback;
  const esc = value => String(value ?? "—").replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;").replaceAll('"', "&quot;").replaceAll("'", "&#039;");
  const clamp = (value, lo, hi) => Math.max(lo, Math.min(hi, value));
  const wrap180 = value => ((Number(value) + 540) % 360) - 180;
  const wrap360 = value => ((Number(value) % 360) + 360) % 360;
  const radians = value => Number(value) * Math.PI / 180;
  const degrees = value => Number(value) * 180 / Math.PI;
  const epoch = (data, info) => {
    const direct = n(data?.timestamp?.epoch_millis);
    if (finite(direct) && direct > 0) return direct;
    const sec = n(info?.source_timestamp?.sec);
    const ns = n(info?.source_timestamp?.nanosec, 0);
    return finite(sec) ? sec * 1000 + ns / 1e6 : Date.now();
  };
  const clock = value => finite(value) ? new Date(Number(value)).toLocaleTimeString([], { hour12: false, hour: "2-digit", minute: "2-digit", second: "2-digit", fractionalSecondDigits: 3 }) : "—";
  const enumName = (value, prefix = "") => String(value ?? "UNKNOWN").replace(prefix, "");
  const median = values => quantile(values, .5);
  function quantile(values, q) {
    const sorted = values.map(Number).filter(Number.isFinite).sort((a, b) => a - b);
    if (!sorted.length) return NaN;
    const p = (sorted.length - 1) * q, lo = Math.floor(p), hi = Math.ceil(p);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * (p - lo);
  }
  const popcount = value => { let x = Number(value) >>> 0, count = 0; while (x) { x &= x - 1; count++; } return count; };
  const guid = info => String(info?.publication_handle ?? info?.publication_virtual_guid ?? info?.instance_handle ?? "writer-unknown");
  const valid = info => info?.valid_data !== false;
  const trim = (array, cutoff, getter = item => item.t) => { while (array.length && getter(array[0]) < cutoff) array.shift(); return array; };
  const nearest = (array, at, getter = item => item.t) => {
    if (!array.length) return null;
    let best = array[0], delta = Math.abs(getter(best) - at);
    for (const item of array) { const d = Math.abs(getter(item) - at); if (d < delta) { best = item; delta = d; } }
    return best;
  };
  function interpolateShip(samples, at) {
    if (!samples.length) return null;
    let a = samples[0], b = samples[samples.length - 1];
    for (let i = 1; i < samples.length; i++) if (samples[i].t >= at) { a = samples[i - 1]; b = samples[i]; break; }
    if (a === b || b.t === a.t) return { ...a, interpolated: false };
    const u = clamp((at - a.t) / (b.t - a.t), 0, 1);
    const lerp = key => n(a.data[key], 0) + (n(b.data[key], 0) - n(a.data[key], 0)) * u;
    const heading = wrap360(n(a.data.heading_deg, 0) + wrap180(n(b.data.heading_deg, 0) - n(a.data.heading_deg, 0)) * u);
    return { t: at, data: { ...a.data, latitude_deg: lerp("latitude_deg"), longitude_deg: lerp("longitude_deg"), altitude_m: lerp("altitude_m"), heading_deg: heading, course_deg: lerp("course_deg"), speed_mps: lerp("speed_mps"), pitch_deg: lerp("pitch_deg"), roll_deg: lerp("roll_deg") }, interpolated: true };
  }
  function localEnu(origin, point) {
    if (!origin || !point) return { east: 0, north: 0, up: 0 };
    const lat = radians(origin.latitude_deg), r = 6378137;
    return { east: radians(n(point.longitude_deg) - n(origin.longitude_deg)) * r * Math.cos(lat), north: radians(n(point.latitude_deg) - n(origin.latitude_deg)) * r, up: n(point.altitude_m) - n(origin.altitude_m) };
  }
  function writerSample(messageSample) {
    return { data: messageSample?.data, info: messageSample?.read_sample_info || {} };
  }

  class WisClient {
    constructor(options) { this.options = options; this.socket = null; this.connectionName = ""; this.endpoint = ""; }
    participantUri() { const o = this.options; return `/dds/rest1/applications/${enc(o.application)}/domain_participants/${enc(o.participant)}`; }
    readerUri(reader) { const o = this.options; return `${this.participantUri()}/subscribers/${enc(o.subscriber)}/data_readers/${enc(reader)}`; }
    headers(content = false) { const h = {}; if (content) h["Content-Type"] = "application/dds-web+json"; if (this.options.apiKey) h["OMG-DDS-API-Key"] = this.options.apiKey; return h; }
    status(state, text) { this.options.onStatus?.(state, text); }
    async enable() {
      const p = this.participantUri(), s = `${p}/subscribers/${enc(this.options.subscriber)}`;
      const uris = [p, s, ...Object.values(this.options.readers).map(reader => this.readerUri(reader))];
      for (const uri of uris) {
        const response = await fetch(this.endpoint + uri, { method: "PUT", headers: this.headers() });
        if (!response.ok) throw new Error(`Enable failed (${response.status}): ${uri}`);
      }
    }
    async bind() {
      for (const [bindId, reader] of Object.entries(this.options.readers)) {
        this.socket.send(JSON.stringify({ kind: "bind", body: [{ bind_kind: "bind_datareader", bind_id: bindId, uri: this.readerUri(reader) }] }));
        await sleep(250);
      }
    }
    async connect(endpoint) {
      await this.disconnect();
      this.endpoint = String(endpoint).replace(/\/+$/, "");
      this.status("connecting", "Enabling DDS readers");
      await this.enable();
      this.connectionName = `radar-${Date.now()}-${Math.random().toString(16).slice(2, 7)}`;
      const created = await fetch(`${this.endpoint}/dds/v1/websocket_connections`, { method: "POST", headers: this.headers(true), body: JSON.stringify([{ name: this.connectionName }]) });
      if (!created.ok && created.status !== 409) throw new Error(`WebSocket creation failed: HTTP ${created.status}`);
      const url = new URL(this.endpoint), protocol = url.protocol === "https:" ? "wss:" : "ws:";
      this.socket = new WebSocket(`${protocol}//${url.host}/dds/websocket/${enc(this.connectionName)}`);
      return new Promise((resolve, reject) => {
        let settled = false;
        this.socket.addEventListener("open", () => this.socket.send(`Content-Type:application/dds-web+json\r\nAccept:application/dds-web+json\r\nOMG-DDS-API-Key:${this.options.apiKey || ""}\r\nVersion:1\r\n\r\n`));
        this.socket.addEventListener("message", async event => {
          const text = typeof event.data === "string" ? event.data : "";
          if (/^HELLO(?:_| )OK/i.test(text)) {
            try { await this.bind(); this.status("live", `${Object.keys(this.options.readers).length} readers bound`); settled = true; resolve(); } catch (error) { reject(error); }
            return;
          }
          if (/^HELLO(?:_| )FAIL/i.test(text)) { const error = new Error(text); this.status("error", "WIS handshake failed"); if (!settled) reject(error); return; }
          try {
            const message = JSON.parse(text);
            if (message.kind !== "b_push") return;
            for (const sample of message.body?.read_sample_seq || []) {
              const parsed = writerSample(sample);
              this.options.onSample?.(message.bind_id, parsed.data, parsed.info);
            }
          } catch (error) { console.error("WIS message error", error, text); }
        });
        this.socket.addEventListener("error", () => { this.status("error", "WIS connection error"); if (!settled) reject(new Error("Web Integration Service connection failed")); });
        this.socket.addEventListener("close", () => this.status("paused", "WIS disconnected"));
      });
    }
    async disconnect() {
      const endpoint = this.endpoint, name = this.connectionName;
      try { this.socket?.close(); } catch { /* best effort */ }
      this.socket = null;
      if (endpoint && name) fetch(`${endpoint}/dds/v1/websocket_connections/${enc(name)}`, { method: "DELETE", headers: this.headers() }).catch(() => {});
      this.connectionName = "";
    }
  }

  function statusBinder(dotId, textId) {
    return (state, text) => {
      const dot = document.getElementById(dotId), label = document.getElementById(textId);
      if (dot) dot.className = `dot ${state === "live" ? "live" : state === "error" ? "error" : ""}`;
      if (label) label.textContent = text;
    };
  }
  function sparkline(canvas, series, options = {}) {
    if (!canvas) return;
    const rect = canvas.getBoundingClientRect(), dpr = global.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(rect.width * dpr)); canvas.height = Math.max(1, Math.round(rect.height * dpr));
    const ctx = canvas.getContext("2d"); ctx.scale(dpr, dpr);
    const w = rect.width, h = rect.height, pad = 28;
    ctx.fillStyle = "#071218"; ctx.fillRect(0, 0, w, h);
    const all = series.flatMap(s => s.values.map(p => p.y)).filter(Number.isFinite);
    if (!all.length) { ctx.fillStyle = "#6f9199"; ctx.font = "12px Segoe UI"; ctx.fillText(options.empty || "Waiting for samples", 12, 22); return; }
    const xs = series.flatMap(s => s.values.map(p => p.x)).filter(Number.isFinite), xmin = Math.min(...xs), xmax = Math.max(...xs) || xmin + 1;
    let ymin = finite(options.min) ? Number(options.min) : Math.min(...all), ymax = finite(options.max) ? Number(options.max) : Math.max(...all);
    if (ymax === ymin) { ymax += 1; ymin -= 1; }
    ctx.strokeStyle = "#17343d"; ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) { const y = pad + (h - pad * 1.5) * i / 4; ctx.beginPath(); ctx.moveTo(pad, y); ctx.lineTo(w - 8, y); ctx.stroke(); }
    ctx.fillStyle = "#6f9199"; ctx.font = "10px Cascadia Mono"; ctx.fillText(fmt(ymax, 1), 2, pad); ctx.fillText(fmt(ymin, 1), 2, h - 10);
    for (const s of series) {
      ctx.strokeStyle = s.color || "#5ce1dc"; ctx.lineWidth = s.width || 1.5; ctx.beginPath(); let first = true;
      for (const p of s.values) { if (!finite(p.x) || !finite(p.y)) continue; const x = pad + (w - pad - 8) * (p.x - xmin) / Math.max(1, xmax - xmin); const y = pad + (h - pad * 1.5) * (1 - (p.y - ymin) / (ymax - ymin)); if (first) { ctx.moveTo(x, y); first = false; } else ctx.lineTo(x, y); }
      ctx.stroke();
    }
  }
  function downloadCsv(filename, rows) {
    if (!rows.length) return;
    const keys = Object.keys(rows[0]), quote = value => `"${String(value ?? "").replaceAll('"', '""')}"`;
    const csv = [keys.map(quote).join(","), ...rows.map(row => keys.map(key => quote(row[key])).join(","))].join("\r\n");
    const a = document.createElement("a"); a.href = URL.createObjectURL(new Blob([csv], { type: "text/csv" })); a.download = filename; a.click(); URL.revokeObjectURL(a.href);
  }

  global.RadarLive = { WisClient, sleep, finite, n, fmt, integer, esc, clamp, wrap180, wrap360, radians, degrees, epoch, clock, enumName, median, quantile, popcount, guid, valid, trim, nearest, interpolateShip, localEnu, statusBinder, sparkline, downloadCsv };
})(window);
