/* disk_setup.js — the browser's half of the setup flow.
 *
 * The screen, the disk identity, and the publication all live in the product's
 * own code (src/platform/web_setup.cpp and the shared setup screen). What only
 * the browser can do is open its file chooser, so this module does exactly that:
 * it opens one <input type="file">, writes what the player picked into the
 * module's filesystem under the directory the native side names, and reports the
 * document names back. Nothing here decides whether a file is a disk image, and
 * nothing here reads ZIP archives: that is the same validator the desktop and
 * Android products use.
 *
 * The native side drives it: benefactorWebPickFiles(directory) is called when the
 * player presses Choose files on the in-canvas setup screen, and every pick must
 * answer exactly once — including when the player cancels, or the screen would
 * wait forever for another attempt. Answers are queued rather than run
 * concurrently, so a second pick can never mix its documents into the first
 * one's answer. */
(() => {
  "use strict";

  const IMPORT_BUDGET_BYTES = 64 * 1024 * 1024;

  function status(message) {
    const element = document.getElementById("disk-status");
    if (element) {
      element.textContent = message;
    }
  }

  function filesystem() {
    // A non-modularized Emscripten build exposes FS as a global; a modularized
    // one attaches it to Module.
    if (typeof FS !== "undefined" && FS !== null) {
      return FS;
    }
    const fromModule = typeof Module !== "undefined" ? Module.FS : undefined;
    return fromModule === undefined ? null : fromModule;
  }

  // Crossing into the module passes strings as pointers, so the signature of
  // each exported call is declared here rather than left to a JavaScript
  // coercion that would silently pass a number where a C string is expected.
  const EXPORTS = Object.freeze({
    begin: { name: "benefactor_web_pick_begin", signature: [] },
    add: { name: "benefactor_web_pick_add", signature: ["string"] },
    end: { name: "benefactor_web_pick_end", signature: [] },
  });

  function call(entry, ...args) {
    if (typeof Module === "undefined" || typeof Module.ccall !== "function") {
      return;
    }
    Module.ccall(entry.name, null, entry.signature, args);
  }

  let input = null;
  let directory = "";
  let queue = Promise.resolve();

  // Every answer is one begin/add*/end sequence, and no two answers overlap.
  function answer(work) {
    queue = queue.then(work);
    return queue;
  }

  function answer_with_no_files(message) {
    return answer(() => {
      call(EXPORTS.begin);
      call(EXPORTS.end);
      status(message);
    });
  }

  async function deliver(files) {
    let total = 0;
    for (const file of files) {
      total += file.size;
    }
    if (total > IMPORT_BUDGET_BYTES) {
      return answer_with_no_files("Those files are larger than this setup accepts.");
    }
    const fs = filesystem();
    if (fs === null) {
      return answer_with_no_files("The page could not open its own filesystem.");
    }
    return answer(async () => {
      call(EXPORTS.begin);
      try {
        fs.mkdirTree(directory);
        for (const file of files) {
          const bytes = new Uint8Array(await file.arrayBuffer());
          fs.writeFile(`${directory}/${file.name}`, bytes);
          call(EXPORTS.add, file.name);
        }
        status("");
      } catch (error) {
        // The answer still ends: whatever reached the filesystem is what the
        // product validates, and the player can choose again. The caught value
        // may come from the module's own realm, so its message is read by shape
        // rather than by an instanceof check that would reject it.
        const reason =
          error !== null && typeof error === "object" && typeof error.message === "string"
            ? error.message
            : "The chosen files could not be read.";
        status(reason);
      }
      call(EXPORTS.end);
    });
  }

  function chooser() {
    if (input !== null) {
      return input;
    }
    input = document.createElement("input");
    input.id = "disk-files";
    input.type = "file";
    input.multiple = true;
    // Disk images use numeric suffixes rather than media extensions, so the
    // chooser stays unrestricted; identity is checked after the files arrive.
    input.accept = "";
    input.style.display = "none";
    input.addEventListener("change", () => {
      const files = input.files ? Array.from(input.files) : [];
      input.value = "";
      return files.length > 0
        ? deliver(files)
        : answer_with_no_files("No files were chosen.");
    });
    input.addEventListener("cancel", () =>
      answer_with_no_files("No files were chosen."),
    );
    document.body.appendChild(input);
    return input;
  }

  globalThis.benefactorWebPickFiles = (target) => {
    directory = target;
    chooser().click();
  };

  globalThis.benefactorWebStatus = (message) => {
    status(message);
  };
})();
