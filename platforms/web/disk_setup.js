(() => {
  "use strict";

  const EXPECTED = Object.freeze({
    "Disk.1": { size: 1003520, sha256: "25416a6e390cbe94e4b2375c9513a2adf3411072fc5b6069ea34a0f3ff697916" },
    "Disk.2": { size: 1003520, sha256: "f3649c8db4adfce3c7da5e21cb018be098404771eceeec44741c2528e9071b73" },
    "Disk.3": { size: 1003520, sha256: "8dd262d02174a6706d5214b25f7bd9fc4bffe94761e16c209b880bc1dd8e7a42" },
  });

  const input = document.getElementById("disk-files");
  const status = document.getElementById("disk-status");
  let lastValid = null;

  async function digest(file) {
    const bytes = await file.arrayBuffer();
    const hash = await crypto.subtle.digest("SHA-256", bytes);
    return {
      file,
      bytes: new Uint8Array(bytes),
      sha256: [...new Uint8Array(hash)].map((value) => value.toString(16).padStart(2, "0")).join(""),
    };
  }

  function fail(message) {
    status.textContent = message;
    input.value = "";
  }

  async function select(files) {
    const byName = new Map();
    for (const file of files) {
      if (!Object.hasOwn(EXPECTED, file.name) || byName.has(file.name)) {
        fail("Select exactly one valid Disk.1, Disk.2, and Disk.3 file.");
        return;
      }
      byName.set(file.name, file);
    }
    if (byName.size !== 3) {
      fail("All three files are required: Disk.1, Disk.2, and Disk.3.");
      return;
    }

    const validated = {};
    for (const [name, expected] of Object.entries(EXPECTED)) {
      const file = byName.get(name);
      if (file.size !== expected.size) {
        fail(`${name} has the wrong size.`);
        return;
      }
      const result = await digest(file);
      if (result.sha256 !== expected.sha256) {
        fail(`${name} does not match the supported disk identity.`);
        return;
      }
      validated[name] = result;
    }

    // Commit only after all three files pass. The native Emscripten bridge owns the bytes.
    lastValid = validated;
    status.textContent = "Disk set verified. Starting Benefactor…";
    window.dispatchEvent(new CustomEvent("benefactor-disks-validated", { detail: validated }));
  }

  input.addEventListener("change", () => {
    select([...input.files]).catch(() => fail("The browser could not read the selected disks."));
  });
  window.benefactorDiskSelection = { getLastValid: () => lastValid };
})();
