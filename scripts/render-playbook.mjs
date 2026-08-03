import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url));
const repositoryRoot = path.resolve(scriptDirectory, "..");
const sourceName = process.argv[2] || "RadarDemoPresentationPlaybook.md";
const outputName = process.argv[3]
    || `${path.basename(sourceName, path.extname(sourceName))}.html`;
const sourcePath = path.resolve(repositoryRoot, sourceName);
const outputPath = path.resolve(repositoryRoot, outputName);
const source = fs.readFileSync(sourcePath, "utf8").replace(/\r\n?/g, "\n");

const escapeHtml = value => String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");

const slugCounts = new Map();
const headings = [];

function slugFor(text) {
    const base = text
        .toLowerCase()
        .replace(/`|\*|_/g, "")
        .replace(/[^a-z0-9]+/g, "-")
        .replace(/^-|-$/g, "") || "section";
    const count = slugCounts.get(base) || 0;
    slugCounts.set(base, count + 1);
    return count ? `${base}-${count + 1}` : base;
}

function inlineMarkdown(value) {
    const code = [];
    let text = String(value).replace(/`([^`]+)`/g, (_, content) => {
        const token = `\u0000CODE${code.length}\u0000`;
        code.push(`<code>${escapeHtml(content)}</code>`);
        return token;
    });
    text = escapeHtml(text);
    text = text.replace(/!\[([^\]]*)]\(([^)]+)\)/g,
        (_, alt, source) => `<img src="${escapeHtml(source)}" alt="${alt}">`);
    text = text.replace(/\[([^\]]+)]\(([^)]+)\)/g,
        (_, label, href) => `<a href="${escapeHtml(href)}">${label}</a>`);
    text = text.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    text = text.replace(/(?<!\*)\*([^*]+)\*(?!\*)/g, "<em>$1</em>");
    text = text.replace(/\u0000CODE(\d+)\u0000/g,
        (_, index) => code[Number(index)]);
    return text;
}

function splitTableRow(line) {
    let value = line.trim();
    if (value.startsWith("|")) value = value.slice(1);
    if (value.endsWith("|")) value = value.slice(0, -1);
    return value.split("|").map(cell => cell.trim());
}

function listMatch(line) {
    const match = /^(\s*)([-+*]|\d+\.)\s+(.*)$/.exec(line);
    if (!match) return null;
    return {
        indent: match[1].length,
        ordered: /\d+\./.test(match[2]),
        content: match[3]
    };
}

const lines = source.split("\n");

function renderList(start, indent, ordered) {
    const tag = ordered ? "ol" : "ul";
    const items = [];
    let index = start;

    while (index < lines.length) {
        while (index < lines.length && !lines[index].trim()) {
            const next = lines.slice(index + 1).find(line => line.trim());
            const nextItem = next ? listMatch(next) : null;
            if (!nextItem || nextItem.indent < indent) {
                return { html: `<${tag}>${items.join("")}</${tag}>`, index };
            }
            index++;
        }

        const item = listMatch(lines[index]);
        if (!item || item.indent !== indent || item.ordered !== ordered) break;

        const content = [item.content];
        const children = [];
        index++;

        while (index < lines.length) {
            if (!lines[index].trim()) {
                let lookahead = index + 1;
                while (lookahead < lines.length && !lines[lookahead].trim()) lookahead++;
                const nextItem = lookahead < lines.length
                    ? listMatch(lines[lookahead]) : null;
                if (nextItem && nextItem.indent > indent) {
                    index = lookahead;
                    const nested = renderList(index, nextItem.indent, nextItem.ordered);
                    children.push(nested.html);
                    index = nested.index;
                    continue;
                }
                if (nextItem && nextItem.indent === indent
                        && nextItem.ordered === ordered) {
                    index = lookahead;
                }
                break;
            }

            const nextItem = listMatch(lines[index]);
            if (nextItem) {
                if (nextItem.indent > indent) {
                    const nested = renderList(
                        index, nextItem.indent, nextItem.ordered);
                    children.push(nested.html);
                    index = nested.index;
                    continue;
                }
                break;
            }

            const leading = /^(\s*)/.exec(lines[index])[1].length;
            if (leading > indent) {
                content.push(lines[index].trim());
                index++;
                continue;
            }
            break;
        }

        items.push(`<li>${inlineMarkdown(content.join(" "))}${children.join("")}</li>`);
    }

    return { html: `<${tag}>${items.join("")}</${tag}>`, index };
}

function startsBlock(index) {
    const line = lines[index] || "";
    if (!line.trim()) return true;
    if (/^#{1,6}\s+/.test(line)) return true;
    if (/^```/.test(line)) return true;
    if (/^>\s?/.test(line)) return true;
    if (listMatch(line)) return true;
    if (line.includes("|") && index + 1 < lines.length
            && /^\s*\|?\s*:?-{3,}/.test(lines[index + 1])) return true;
    return false;
}

function renderMarkdown() {
    const output = [];
    let index = 0;

    while (index < lines.length) {
        const line = lines[index];
        if (!line.trim()) {
            index++;
            continue;
        }

        const fence = /^```(.*)$/.exec(line);
        if (fence) {
            const language = fence[1].trim();
            const body = [];
            index++;
            while (index < lines.length && !/^```/.test(lines[index])) {
                body.push(lines[index]);
                index++;
            }
            if (index < lines.length) index++;
            const languageClass = language
                ? ` class="language-${escapeHtml(language)}"` : "";
            output.push(`<pre><code${languageClass}>${escapeHtml(body.join("\n"))}</code></pre>`);
            continue;
        }

        const heading = /^(#{1,6})\s+(.*)$/.exec(line);
        if (heading) {
            const level = heading[1].length;
            const text = heading[2].trim();
            const id = slugFor(text);
            if (level === 2) headings.push({ id, text: text.replace(/[`*_]/g, "") });
            output.push(`<h${level} id="${id}">${inlineMarkdown(text)}</h${level}>`);
            index++;
            continue;
        }

        if (line.includes("|") && index + 1 < lines.length
                && /^\s*\|?\s*:?-{3,}/.test(lines[index + 1])) {
            const headers = splitTableRow(line);
            const alignment = splitTableRow(lines[index + 1]).map(cell => {
                const left = cell.startsWith(":"), right = cell.endsWith(":");
                return left && right ? "center" : right ? "right" : left ? "left" : "left";
            });
            index += 2;
            const rows = [];
            while (index < lines.length && lines[index].trim().includes("|")) {
                rows.push(splitTableRow(lines[index]));
                index++;
            }
            const head = headers.map((cell, column) =>
                `<th style="text-align:${alignment[column] || "left"}">${inlineMarkdown(cell)}</th>`).join("");
            const body = rows.map(row => `<tr>${row.map((cell, column) =>
                `<td style="text-align:${alignment[column] || "left"}">${inlineMarkdown(cell)}</td>`).join("")}</tr>`).join("");
            output.push(`<div class="table-wrap"><table><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table></div>`);
            continue;
        }

        if (/^>\s?/.test(line)) {
            const quote = [];
            while (index < lines.length && /^>\s?/.test(lines[index])) {
                quote.push(lines[index].replace(/^>\s?/, "").trim());
                index++;
            }
            output.push(`<blockquote><p>${inlineMarkdown(quote.join(" "))}</p></blockquote>`);
            continue;
        }

        const item = listMatch(line);
        if (item) {
            const list = renderList(index, item.indent, item.ordered);
            output.push(list.html);
            index = list.index;
            continue;
        }

        const paragraph = [line.trim()];
        index++;
        while (index < lines.length && !startsBlock(index)) {
            paragraph.push(lines[index].trim());
            index++;
        }
        output.push(`<p>${inlineMarkdown(paragraph.join(" "))}</p>`);
    }

    return output.join("\n");
}

const rendered = renderMarkdown();
const titleMatch = /^#\s+(.+)$/m.exec(source);
const title = titleMatch ? titleMatch[1] : "Connext Studio Demonstration Playbook";
const toc = headings.map(({ id, text }) =>
    `<li><a href="#${id}">${escapeHtml(text)}</a></li>`).join("\n");

const html = `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escapeHtml(title)} — Demonstration Playbook</title>
  <style>
    :root { color-scheme: light; --ink:#17252b; --muted:#52666e; --navy:#082b49; --cyan:#147e91; --pale:#edf5f6; --line:#cbdadd; --paper:#fff; --code:#102830; }
    * { box-sizing:border-box; }
    html { scroll-behavior:smooth; }
    body { margin:0; background:#e8eff1; color:var(--ink); font:15px/1.58 "Segoe UI", Arial, sans-serif; }
    .print-bar { position:sticky; top:0; z-index:10; display:flex; justify-content:space-between; align-items:center; gap:16px; padding:9px max(18px,calc((100vw - 1120px)/2)); color:#d8eef3; background:#071e2d; border-bottom:1px solid #214454; }
    .print-bar span { color:#9eb9c1; font-size:12px; }
    button { border:1px solid #57aab6; border-radius:5px; padding:7px 13px; color:white; background:#146e7c; font-weight:650; cursor:pointer; }
    .sheet { width:min(1120px,calc(100% - 28px)); margin:24px auto 46px; background:var(--paper); box-shadow:0 12px 36px #17394324; }
    .cover { min-height:340px; display:flex; flex-direction:column; justify-content:flex-end; padding:58px 64px; color:white; background:linear-gradient(135deg,#061d32 0%,#0b4660 70%,#168596 100%); }
    .kicker { margin-bottom:14px; color:#8fe7ef; font-size:12px; font-weight:750; letter-spacing:.18em; text-transform:uppercase; }
    .cover h1 { max-width:780px; margin:0; font-size:clamp(38px,6vw,68px); line-height:1.02; letter-spacing:-.045em; }
    .cover p { max-width:720px; margin:22px 0 0; color:#d2e8ed; font-size:18px; }
    .toc { padding:34px 64px; background:#f4f8f9; border-bottom:1px solid var(--line); }
    .toc h2 { margin:0 0 16px; border:0; font-size:17px; }
    .toc ul { columns:2; column-gap:40px; margin:0; padding:0; list-style:none; }
    .toc li { break-inside:avoid; margin:5px 0; }
    .toc a { color:#155d6b; text-decoration:none; }
    article { padding:46px 64px 70px; }
    article > h1:first-child { display:none; }
    h2 { margin:54px 0 18px; padding-bottom:8px; color:var(--navy); border-bottom:2px solid #78bbc4; font-size:27px; line-height:1.2; }
    h2:first-of-type { margin-top:0; }
    h3 { margin:30px 0 10px; color:#145e6d; font-size:19px; }
    p { margin:10px 0 15px; }
    ul,ol { margin:10px 0 18px; padding-left:27px; }
    li { margin:5px 0; }
    li > ul, li > ol { margin:6px 0 8px; }
    a { color:#096c80; text-decoration-thickness:1px; text-underline-offset:2px; }
    strong { color:#0d3f4d; }
    code { padding:.1em .32em; border-radius:3px; color:#0d5664; background:#eaf3f4; font:90%/1.4 Consolas,"Cascadia Mono",monospace; overflow-wrap:anywhere; }
    pre { margin:15px 0 20px; padding:16px 18px; overflow:auto; border-left:4px solid #41a6b2; border-radius:4px; color:#e4f3f4; background:var(--code); box-shadow:inset 0 0 0 1px #28434b; }
    pre code { padding:0; color:inherit; background:transparent; overflow-wrap:normal; }
    blockquote { margin:17px 0 22px; padding:14px 18px; border-left:4px solid #e49c32; color:#314d55; background:#fff7e8; }
    blockquote p { margin:0; }
    article img { display:block; width:100%; max-width:100%; height:auto; margin:18px auto 25px; padding:8px; border:1px solid var(--line); border-radius:5px; background:white; }
    .table-wrap { margin:16px 0 24px; overflow-x:auto; border:1px solid var(--line); border-radius:5px; }
    table { width:100%; border-collapse:collapse; font-size:13px; }
    th { color:white; background:#164f61; font-size:11px; letter-spacing:.035em; text-transform:uppercase; }
    th,td { padding:9px 10px; vertical-align:top; border-right:1px solid #d8e3e5; border-bottom:1px solid #d8e3e5; }
    th:last-child,td:last-child { border-right:0; }
    tbody tr:nth-child(even) { background:#f5f9f9; }
    tbody tr:last-child td { border-bottom:0; }
    @media (max-width:720px) { .cover,.toc,article { padding-left:24px; padding-right:24px; } .toc ol { columns:1; } }
    @media print {
      @page { size:letter; margin:.58in .58in .62in; }
      body { background:white; font-size:10.3pt; line-height:1.43; }
      .print-bar { display:none; }
      .sheet { width:auto; margin:0; box-shadow:none; }
      .cover { min-height:8.2in; padding:.55in; break-after:page; print-color-adjust:exact; -webkit-print-color-adjust:exact; }
      .cover h1 { font-size:46pt; }
      .toc { padding:0; background:white; border:0; break-after:page; }
      .toc ul { columns:2; }
      article { padding:0; }
      h2 { margin-top:26px; font-size:19pt; break-after:avoid; }
      h3 { break-after:avoid; }
      pre,blockquote,table,tr { break-inside:avoid; }
      pre { white-space:pre-wrap; overflow-wrap:anywhere; print-color-adjust:exact; -webkit-print-color-adjust:exact; }
      th { print-color-adjust:exact; -webkit-print-color-adjust:exact; }
      a { color:inherit; text-decoration:none; }
    }
  </style>
</head>
<body>
  <div class="print-bar"><span>Standalone HTML generated from ${escapeHtml(path.basename(sourcePath))}</span><button type="button" onclick="window.print()">Print / Save PDF</button></div>
  <main class="sheet">
    <header class="cover">
      <div class="kicker">Operator playbook · Live DDS demonstration</div>
      <h1>${escapeHtml(title)}</h1>
      <p>Scenario preparation, live observation, presenter prompts, restoration steps, and troubleshooting for the AESA radar simulation.</p>
    </header>
    <nav class="toc" aria-label="Contents"><h2>Contents</h2><ul>${toc}</ul></nav>
    <article>${rendered}</article>
  </main>
</body>
</html>
`;

fs.writeFileSync(outputPath, html, "utf8");
console.log(`Rendered ${path.relative(repositoryRoot, sourcePath)} -> ${path.relative(repositoryRoot, outputPath)}`);
