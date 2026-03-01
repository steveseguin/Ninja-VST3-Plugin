const owner = "steveseguin";
const repo = "Ninja-VST3-Plugin";
const fixedInstallerAssetName = "webrtc_vst-windows-setup.exe";
const releasesApi = `https://api.github.com/repos/${owner}/${repo}/releases`;
const latestReleaseUrl = `https://github.com/${owner}/${repo}/releases/latest`;
const latestInstallerUrl = `https://github.com/${owner}/${repo}/releases/latest/download/${fixedInstallerAssetName}`;

function formatDate(isoDate) {
  if (!isoDate) {
    return "Unknown date";
  }
  return new Date(isoDate).toLocaleDateString(undefined, {
    year: "numeric",
    month: "short",
    day: "numeric"
  });
}

function formatBytes(bytes) {
  const safeBytes = Number.isFinite(bytes) ? bytes : 0;
  if (safeBytes < 1024) {
    return `${safeBytes} B`;
  }
  if (safeBytes < 1024 * 1024) {
    return `${(safeBytes / 1024).toFixed(1)} KB`;
  }
  return `${(safeBytes / (1024 * 1024)).toFixed(1)} MB`;
}

function firstLine(text) {
  if (!text || typeof text !== "string") {
    return "";
  }
  return text.split(/\r?\n/).find((line) => line.trim().length > 0) || "";
}

function renderAssets(assets) {
  if (!assets || assets.length === 0) {
    return "<p class=\"empty\">No binary assets published for this release.</p>";
  }

  const items = assets.map((asset) => {
    const safeName = asset.name || "asset";
    const safeUrl = asset.browser_download_url || latestReleaseUrl;
    return `<li>
      <a href="${safeUrl}">${safeName}</a>
      <span class="asset-size">${formatBytes(asset.size || 0)}</span>
    </li>`;
  }).join("");

  return `<ul class="release-assets">${items}</ul>`;
}

function renderReleaseCard(release) {
  const name = release.name || release.tag_name || "Release";
  const tag = release.tag_name || "untagged";
  const publishedAt = formatDate(release.published_at || release.created_at);
  const notes = firstLine(release.body);
  const assetsHtml = renderAssets(release.assets || []);
  const notesHtml = notes
    ? `<p class="release-notes">${notes}</p>`
    : "";

  return `<article class="release-card">
    <div class="release-title-row">
      <h3>${name}</h3>
      <span class="release-tag">${tag}</span>
    </div>
    <p class="release-date">${publishedAt}</p>
    ${assetsHtml}
    ${notesHtml}
  </article>`;
}

function setLatestReleaseUi(release) {
  const latestVersion = document.getElementById("latestVersion");
  const latestDate = document.getElementById("latestDate");
  const downloadLatest = document.getElementById("downloadLatest");

  const releaseName = release.name || release.tag_name || "latest";
  latestVersion.textContent = `Latest release: ${releaseName}`;
  latestDate.textContent = `Published: ${formatDate(release.published_at || release.created_at)}`;

  const installerAsset = (release.assets || []).find((asset) => asset.name === fixedInstallerAssetName);
  if (installerAsset && installerAsset.browser_download_url) {
    downloadLatest.href = installerAsset.browser_download_url;
    return;
  }
  downloadLatest.href = latestInstallerUrl || latestReleaseUrl;
}

async function loadReleases() {
  const releaseList = document.getElementById("releaseList");

  try {
    const response = await fetch(releasesApi, {
      headers: {
        Accept: "application/vnd.github+json"
      }
    });

    if (!response.ok) {
      throw new Error(`GitHub API request failed (${response.status})`);
    }

    const releases = await response.json();
    const published = releases.filter((release) => !release.draft);

    if (published.length === 0) {
      releaseList.innerHTML = "<p class=\"empty\">No releases published yet. Check back after the first tagged build.</p>";
      const latestVersion = document.getElementById("latestVersion");
      latestVersion.textContent = "Latest release: none yet";
      return;
    }

    setLatestReleaseUi(published[0]);
    releaseList.innerHTML = published.slice(0, 12).map(renderReleaseCard).join("");
  } catch (error) {
    releaseList.innerHTML = "<p class=\"error\">Unable to load releases from GitHub API right now. Use the Browse All Releases link above.</p>";
    console.error(error);
  }
}

document.addEventListener("DOMContentLoaded", loadReleases);
