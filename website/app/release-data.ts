import rendererRelease from '../../engine/renderer-release.json';

const renderer = rendererRelease.remote_binary[0];

if (!renderer) {
  throw new Error('engine/renderer-release.json must define one published MAKO Renderer payload');
}

const repository = renderer.source_repository.replace(/\/$/, '');
const rendererVersion = renderer.version;

export const releaseData = {
  rendererVersion,
  links: {
    repository,
    rendererRelease: `${repository}/releases/tag/${renderer.release_tag}`,
    rendererDownload: renderer.url,
    flatpakDownload: renderer.flatpak_bundle.url,
    docs: `${repository}#install-and-use`,
    issues: `${repository}/issues`,
    allReleases: `${repository}/releases`,
    discord: 'https://discord.gg/NAVkyCq7Rc',
    losslessScaling: 'https://store.steampowered.com/app/993090/Lossless_Scaling/',
  },
} as const;
