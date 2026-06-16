/* Inject a versions dropdown in the mkdocs-material header.
 * Reads `../meta` (JSON {name, latest, versions[]}) from the bucket root
 * for the current software, and lets the user jump between versions. */

(function () {
    "use strict";

    function insertSelector(meta, currentVersion) {
        var header = document.querySelector(".md-header__inner");
        if (!header) return;

        var wrapper = document.createElement("div");
        wrapper.className = "md-header__version-selector";

        var select = document.createElement("select");
        select.setAttribute("aria-label", "Documentation version");

        var versions = (meta.versions || []).slice().reverse();
        versions.forEach(function (v) {
            var opt = document.createElement("option");
            opt.value = v;
            opt.textContent = v;
            if (v === currentVersion) opt.selected = true;
            select.appendChild(opt);
        });

        select.addEventListener("change", function () {
            // URL pattern expected: .../<software>/<version>/<page>
            // Replace the version segment and keep the page (or fall back to index.html).
            var parts = window.location.pathname.split("/");
            var verIdx = parts.length - 2; // last segment is the page
            if (verIdx < 0) return;
            parts[verIdx] = select.value;
            // If the page is empty (trailing slash), default to index.html
            if (!parts[parts.length - 1]) parts[parts.length - 1] = "index.html";
            window.location.pathname = parts.join("/");
        });

        wrapper.appendChild(select);
        header.appendChild(wrapper);
    }

    function currentVersionFromPath() {
        // .../<software>/<version>/<page>  -> version is the segment before last
        var parts = window.location.pathname.split("/").filter(Boolean);
        if (parts.length < 2) return null;
        return parts[parts.length - 2];
    }

    function markHomePage() {
        // Add `mdx-home` body class when the current page is the site index,
        // so extra.css can hide the (empty) primary sidebar.
        var p = window.location.pathname;
        if (p.endsWith("/") || p.endsWith("/index.html")) {
            document.body.classList.add("mdx-home");
        }
    }

    function init() {
        markHomePage();
        fetch("../meta", { cache: "no-store" })
            .then(function (r) { return r.ok ? r.json() : null; })
            .catch(function () { return null; })
            .then(function (meta) {
                if (!meta || !Array.isArray(meta.versions) || meta.versions.length === 0) {
                    return;
                }
                var cur = currentVersionFromPath() || meta.latest;
                insertSelector(meta, cur);
            });
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", init);
    } else {
        init();
    }
})();
