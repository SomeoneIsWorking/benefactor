(() => {
  "use strict";

  const EXPECTED = Object.freeze({
    "Disk.1": { size: 1003520, sha256: "25416a6e390cbe94e4b2375c9513a2adf3411072fc5b6069ea34a0f3ff697916" },
    "Disk.2": { size: 1003520, sha256: "f3649c8db4adfce3c7da5e21cb018be098404771eceeec44741c2528e9071b73" },
    "Disk.3": { size: 1003520, sha256: "8dd262d02174a6706d5214b25f7bd9fc4bffe94761e16c209b880bc1dd8e7a42" },
  });
  const MAX_ZIP_ENTRIES = 128;
  const MAX_ZIP_BYTES = 32 * 1024 * 1024;
  const MAX_ZIP_COMPRESSED = 16 * 1024 * 1024;
  const MAX_ZIP_EXPANDED = 16 * 1024 * 1024;
  const ZIP_EOCD = 0x06054b50;
  const ZIP_CENTRAL = 0x02014b50;
  const ZIP_LOCAL = 0x04034b50;

  const input = document.getElementById("disk-files");
  const status = document.getElementById("disk-status");
  let lastValid = null;
  let phase = "awaiting-disks";

  function readU16(view, offset) {
    return view.getUint16(offset, true);
  }

  function readU32(view, offset) {
    return view.getUint32(offset, true);
  }

  function safeZipName(name) {
    return name && !name.includes("\\") && !name.includes("\0") && !name.startsWith("/") &&
      !name.split("/").includes("..");
  }

  function findZipEnd(view) {
    const first = Math.max(0, view.byteLength - 22 - 65535);
    for (let offset = view.byteLength - 22; offset >= first; --offset) {
      if (readU32(view, offset) === ZIP_EOCD) return offset;
    }
    throw new Error("the selected ZIP has no valid central directory");
  }

  function parseZip(bytes) {
    if (bytes.byteLength > MAX_ZIP_BYTES) {
      throw new Error("the ZIP file is larger than the supported archive limit");
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const end = findZipEnd(view);
    const entries = readU16(view, end + 10);
    const centralSize = readU32(view, end + 12);
    const centralOffset = readU32(view, end + 16);
    if (readU16(view, end + 4) !== 0 || readU16(view, end + 6) !== 0 || entries === 0xffff ||
        centralSize === 0xffffffff || centralOffset === 0xffffffff) {
      throw new Error("ZIP64 and multi-volume ZIP files are not supported");
    }
    if (entries > MAX_ZIP_ENTRIES || centralOffset + centralSize > bytes.byteLength) {
      throw new Error("the ZIP exceeds the supported entry or size limits");
    }

    const decoder = new TextDecoder("utf-8", { fatal: true });
    const result = [];
    let offset = centralOffset;
    let compressedTotal = 0;
    let expandedTotal = 0;
    for (let index = 0; index < entries; ++index) {
      if (offset + 46 > bytes.byteLength || readU32(view, offset) !== ZIP_CENTRAL) {
        throw new Error("the ZIP central directory is corrupt");
      }
      const flags = readU16(view, offset + 8);
      const method = readU16(view, offset + 10);
      const compressedSize = readU32(view, offset + 20);
      const expandedSize = readU32(view, offset + 24);
      const nameSize = readU16(view, offset + 28);
      const extraSize = readU16(view, offset + 30);
      const commentSize = readU16(view, offset + 32);
      const localOffset = readU32(view, offset + 42);
      const nameStart = offset + 46;
      const next = nameStart + nameSize + extraSize + commentSize;
      if (next > bytes.byteLength || compressedSize === 0xffffffff || expandedSize === 0xffffffff ||
          localOffset === 0xffffffff) {
        throw new Error("the ZIP contains an unsupported extended entry");
      }
      let name;
      try {
        name = decoder.decode(bytes.subarray(nameStart, nameStart + nameSize));
      } catch {
        throw new Error("the ZIP contains a non-UTF-8 entry name");
      }
      if (!safeZipName(name) || (flags & 1) !== 0 || (method !== 0 && method !== 8)) {
        throw new Error("the ZIP contains an unsafe, encrypted, or unsupported entry");
      }
      compressedTotal += compressedSize;
      expandedTotal += expandedSize;
      if (compressedTotal > MAX_ZIP_COMPRESSED || expandedTotal > MAX_ZIP_EXPANDED) {
        throw new Error("the ZIP exceeds the supported compressed or expanded size");
      }
      if (localOffset + 30 > bytes.byteLength || readU32(view, localOffset) !== ZIP_LOCAL) {
        throw new Error("the ZIP contains an invalid local entry");
      }
      const localNameSize = readU16(view, localOffset + 26);
      const localExtraSize = readU16(view, localOffset + 28);
      const dataStart = localOffset + 30 + localNameSize + localExtraSize;
      if (dataStart + compressedSize > bytes.byteLength) {
        throw new Error("the ZIP contains truncated entry data");
      }
      if (name.endsWith("/")) {
        offset = next;
        continue;
      }
      result.push({
        basename: name.split("/").at(-1),
        compressedSize,
        expandedSize,
        method,
        dataStart,
      });
      offset = next;
    }
    if (offset !== centralOffset + centralSize) {
      throw new Error("the ZIP central directory size is inconsistent");
    }
    return { bytes, entries: result };
  }

  async function extractZip(file) {
    const archive = parseZip(new Uint8Array(await file.arrayBuffer()));
    const matches = new Map();
    for (const entry of archive.entries) {
      if (!Object.hasOwn(EXPECTED, entry.basename)) continue;
      if (matches.has(entry.basename)) {
        throw new Error(`the ZIP contains duplicate ${entry.basename} entries`);
      }
      matches.set(entry.basename, entry);
    }
    if (matches.size !== Object.keys(EXPECTED).length) {
      throw new Error("the ZIP must contain Disk.1, Disk.2, and Disk.3");
    }

    const extracted = {};
    for (const [name, entry] of matches) {
      const compressed = archive.bytes.slice(entry.dataStart, entry.dataStart + entry.compressedSize);
      if (entry.method === 0) {
        extracted[name] = compressed;
        continue;
      }
      if (typeof DecompressionStream === "undefined") {
        throw new Error("this browser cannot extract deflated ZIP files; select the disks directly");
      }
      const stream = new Blob([compressed]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
      extracted[name] = new Uint8Array(await new Response(stream).arrayBuffer());
      if (extracted[name].byteLength !== entry.expandedSize) {
        throw new Error(`${name} is truncated or corrupt inside the ZIP`);
      }
    }
    return extracted;
  }

  async function readSelected(files) {
    const selected = [...files];
    const archives = selected.filter((file) => file.name.toLowerCase().endsWith(".zip"));
    if (archives.length) {
      if (selected.length !== 1) {
        throw new Error("select either one ZIP or the three disk files, not both");
      }
      return extractZip(archives[0]);
    }
    const byName = {};
    for (const file of selected) {
      if (!Object.hasOwn(EXPECTED, file.name) || byName[file.name]) {
        throw new Error("select exactly one valid Disk.1, Disk.2, and Disk.3 file");
      }
      byName[file.name] = new Uint8Array(await file.arrayBuffer());
    }
    if (Object.keys(byName).length !== Object.keys(EXPECTED).length) {
      throw new Error("all three files are required: Disk.1, Disk.2, and Disk.3");
    }
    return byName;
  }

  async function digest(bytes) {
    const hash = await crypto.subtle.digest("SHA-256", bytes);
    return [...new Uint8Array(hash)].map((value) => value.toString(16).padStart(2, "0")).join("");
  }

  function fail(message) {
    status.textContent = message;
    input.value = "";
    input.disabled = false;
    phase = "awaiting-disks";
  }

  async function select(files) {
    if (phase !== "awaiting-disks") return;
    phase = "validating";
    input.disabled = true;
    let selected;
    try {
      selected = await readSelected(files);
      for (const [name, expected] of Object.entries(EXPECTED)) {
        const bytes = selected[name];
        if (bytes.byteLength !== expected.size) {
          throw new Error(`${name} has the wrong size`);
        }
        if (await digest(bytes) !== expected.sha256) {
          throw new Error(`${name} does not match the supported disk identity`);
        }
      }
    } catch (error) {
      fail(error instanceof Error ? error.message : "The browser could not read the selected disks");
      return;
    }

    // The native entry is one-shot. A failed start may have changed native state,
    // so only pre-commit validation failures can return to disk selection.
    phase = "starting";
    status.textContent = "Disk set verified. Starting Benefactor…";
    try {
      for (const [name, bytes] of Object.entries(selected)) {
        Module.FS.writeFile(`/${name}`, bytes);
      }
      if (Module.ccall("benefactor_web_start", "number", [], []) !== 0) {
        throw new Error("Benefactor could not start from the verified disk set");
      }
    } catch (error) {
      phase = "failed";
      const message = error instanceof Error ? error.message : "Benefactor could not start";
      status.textContent = `${message}. Reload the page to try again.`;
      return;
    }
    lastValid = selected;
    phase = "running";
    status.textContent = "Disk set verified. Reload the page to change disks.";
    window.dispatchEvent(new CustomEvent("benefactor-disks-validated", { detail: selected }));
  }

  input.addEventListener("change", () => select(input.files));
  window.benefactorDiskSelection = { getLastValid: () => lastValid };
})();
