'use client';

import type { PointerEvent as ReactPointerEvent } from 'react';
import { releaseData } from './release-data';
import { DiscordIcon, GitHubIcon } from './social-icons';

const { rendererVersion, links } = releaseData;

const newTabProps = { target: '_blank', rel: 'noopener noreferrer' } as const;

function DonationPanel() {
  return (
    <div className="donate-panel">
      <p><strong>Support framegen-vulkan</strong>Help fund development, GPU compute and real-hardware testing.</p>
      <a href="https://ko-fi.com/eugeniosegala" {...newTabProps}>Ko-fi <span aria-hidden="true">↗</span></a>
      <a href="https://github.com/sponsors/eugeniosegala" {...newTabProps}>GitHub Sponsors <span aria-hidden="true">↗</span></a>
    </div>
  );
}

function DonateMenu() {
  return (
    <details className="donate-menu" onKeyDown={(event) => {
      if (event.key === 'Escape') {
        event.currentTarget.open = false;
        event.currentTarget.querySelector('summary')?.focus();
      }
    }}>
      <summary>Donate <span className="donate-chevron" aria-hidden="true" /></summary>
      <DonationPanel />
    </details>
  );
}

function tiltCard(event: ReactPointerEvent<HTMLDivElement>) {
  if (event.pointerType === 'touch' || window.matchMedia('(prefers-reduced-motion: reduce)').matches) return;

  const card = event.currentTarget;
  const bounds = card.getBoundingClientRect();
  const x = Math.min(1, Math.max(0, (event.clientX - bounds.left) / bounds.width));
  const y = Math.min(1, Math.max(0, (event.clientY - bounds.top) / bounds.height));

  card.style.setProperty('--tilt-x', `${((0.5 - y) * 7).toFixed(2)}deg`);
  card.style.setProperty('--tilt-y', `${((x - 0.5) * 8).toFixed(2)}deg`);
  card.style.setProperty('--shine-x', `${(x * 100).toFixed(1)}%`);
  card.style.setProperty('--shine-y', `${(y * 100).toFixed(1)}%`);
}

function resetCardTilt(event: ReactPointerEvent<HTMLDivElement>) {
  const card = event.currentTarget;
  card.style.setProperty('--tilt-x', '0deg');
  card.style.setProperty('--tilt-y', '0deg');
  card.style.setProperty('--shine-x', '50%');
  card.style.setProperty('--shine-y', '50%');
}

const features = [
  {
    index: '01',
    code: 'AF',
    title: 'Adaptive Frame Generation',
    text: 'Target 30–240 FPS while framegen-vulkan varies generated frames up to your selected 2×–5× ceiling.',
    stat: '30-240',
    label: 'FPS target',
    tone: 'cyan',
  },
  {
    index: '02',
    code: 'SR',
    title: 'LS1 + framegen-vulkan spatial scaling',
    text: 'Choose proprietary LS1 Quality or Performance, or the built-in open framegen-vulkan Scaler, before Fixed or Adaptive generation.',
    stat: '1-2×',
    label: 'scale factor',
    tone: 'violet',
  },
  {
    index: '03',
    code: 'PF',
    title: 'Per-game profiles',
    text: 'Capture a game once. framegen-vulkan selects its isolated renderer and compatibility settings automatically.',
    stat: '1:1',
    label: 'game identity',
    tone: 'orange',
  },
  {
    index: '04',
    code: 'RC',
    title: 'Gamescope recovery',
    text: 'Bounded recovery preserves native presentation, then resumes generation only after cadence stabilizes.',
    stat: 'NATIVE',
    label: 'fallback path',
    tone: 'green',
  },
  {
    index: '05',
    code: 'SH',
    title: 'Per-game shaders',
    text: 'Sharpen, smooth, or stylize each game with lightweight controls that update live while you play.',
    stat: 'LIVE',
    label: 'effect tuning',
    tone: 'blue',
  },
  {
    index: '06',
    code: 'VK',
    title: 'Dual-architecture Vulkan',
    text: 'Architecture-matched 64-bit and 32-bit x86 layers let the Vulkan loader select the correct binary.',
    stat: '32+64',
    label: 'bit layers',
    tone: 'pink',
  },
];

export default function Home() {
  return (
    <main>
      <a className="skip-link" href="#content">Skip to content</a>

      <nav className="site-nav" aria-label="Main navigation">
        <a className="brand" href="#top" aria-label="framegen-vulkan home">
          <img src="assets/fgvk-discord-icon.webp" alt="" width="44" height="44" draggable={false} />
          <span>framegen-vulkan</span>
        </a>
        <div className="nav-links">
          <a href="#system">System</a>
          <a href="#features">Features</a>
          <a href={links.docs} {...newTabProps}>Installation Guide</a>
          <a href="#downloads">Downloads</a>
          <a className="social-link" href={links.repository} {...newTabProps}><GitHubIcon /><span>GitHub</span></a>
          <a className="social-link" href={links.discord} {...newTabProps}><DiscordIcon /><span>Discord</span></a>
          <DonateMenu />
        </div>
        <a className="nav-download" href={links.rendererDownload} {...newTabProps}>
          Download <span>v{rendererVersion}</span>
        </a>
        <details className="mobile-nav">
          <summary>Menu <span aria-hidden="true">+</span></summary>
          <div className="mobile-nav-panel">
            <a href="#system">System</a>
            <a href="#features">Features</a>
            <a href={links.docs} {...newTabProps}>Installation Guide</a>
            <a href="#downloads">Downloads</a>
            <a className="social-link" href={links.repository} {...newTabProps}><GitHubIcon /><span>GitHub</span></a>
            <a className="social-link" href={links.discord} {...newTabProps}><DiscordIcon /><span>Discord</span></a>
            <section className="mobile-donate" aria-label="Donate">
              <DonationPanel />
            </section>
          </div>
        </details>
      </nav>

      <div id="content">
        <section className="hero" id="top">
          <div className="hero-glow hero-glow-cyan" />
          <div className="hero-glow hero-glow-orange" />
          <div className="hero-copy">
            <p className="eyebrow"><span /> Frame generation + spatial scaling + shaders / Linux</p>
            <h1>Frames. Scaling.<br /><em>Shaders.</em></h1>
            <p className="hero-intro">framegen-vulkan is an independent Linux project, unaffiliated with Lossless Scaling. LSFG frame generation and LS1 require a separate Lossless Scaling purchase; the open framegen-vulkan Scaler and bundled shaders work without it.</p>
            <a className="open-source-sticker" href={`${links.repository}/blob/main/LICENSE.md`} {...newTabProps} aria-label="framegen-vulkan is free and open source under the GPL-3.0-or-later licence">
              <span className="open-source-sticker-mark" aria-hidden="true">OPEN</span>
              <span className="open-source-sticker-copy"><strong>Fully open source</strong><span>Contribute. Fork it. Change it. Make it yours.</span></span>
              <span className="open-source-sticker-arrow" aria-hidden="true">↗</span>
            </a>
            <div className="hero-actions">
              <a className="button button-primary button-renderer" href={links.rendererDownload} {...newTabProps}>
                <span className="download-glyph" aria-hidden="true"><i /></span>
                <span className="hero-download-copy"><span>Get framegen-vulkan</span><small>Standalone for desktop Linux</small></span>
              </a>
              <a className="button button-secondary" href={links.docs} {...newTabProps}>Installation Guide <span aria-hidden="true">→</span></a>
              <a className="button button-secondary" href="#system">Explore the system <span aria-hidden="true">↓</span></a>
            </div>
            <div className="hero-meta">
              <span><b>1-2×</b> LS1 or framegen-vulkan Scaler</span>
              <span><b>30-240</b> adaptive FPS target</span>
              <span><b>2-5×</b> generation ceiling</span>
              <span><b>LIVE</b> per-game shader effects</span>
            </div>
          </div>

          <div className="hero-visual" aria-label="framegen-vulkan system visualization">
            <div className="orbit orbit-one" />
            <div className="orbit orbit-two" />
            <div className="visual-card visual-card-renderer" onPointerMove={tiltCard} onPointerLeave={resetCardTilt}>
              <span className="card-index">01 / PIPELINE</span>
              <img src="assets/fgvk-render-logo.webp" alt="framegen-vulkan shark logo" width="154" height="154" draggable={false} />
              <div><strong>framegen-vulkan</strong><span>Scaling + synthesis + shaders</span></div>
            </div>
          </div>
        </section>

        <div className="signal-strip" aria-hidden="true">
          <div>STEAM DECK</div><span />
          <div>STEAM MACHINE</div><span />
          <div>STEAMOS</div><span />
          <div>LINUX</div><span />
          <div>VULKAN</div><span />
          <div>GAMESCOPE</div><span />
          <div>FLATPAK</div>
        </div>

        <section className="system-section section-shell" id="system">
          <header className="section-heading">
            <p className="section-kicker"><span>01</span> One renderer. One pipeline.</p>
            <div>
              <h2>Built on<br /><em>Vulkan.</em></h2>
              <p>framegen-vulkan runs the frame pipeline in a private Vulkan layer, bringing frame generation, spatial scaling, and bundled shader effects into Linux gaming.</p>
            </div>
          </header>

          <div className="system-grid">
            <article className="system-panel renderer-panel">
              <div className="panel-number">01</div>
              <div className="panel-topline"><span>RENDER LAYER</span><i>VULKAN / LINUX</i></div>
              <img src="assets/fgvk-render-logo.webp" alt="framegen-vulkan" width="230" height="230" draggable={false} />
              <h3>framegen-vulkan</h3>
              <p>A private Vulkan pipeline for spatial reconstruction, frame synthesis, deterministic scheduling, and presentation recovery, with optional bundled shader effects.</p>
              <ul>
                <li><span /> Proprietary LS1 or built-in open framegen-vulkan Scaler</li>
                <li><span /> Lossless Scaling frame-generation models</li>
                <li><span /> Adaptive generated-frame planning</li>
              </ul>
              <a href={links.rendererDownload} {...newTabProps}>Download Linux archive <span>→</span></a>
            </article>
          </div>
        </section>

        <section className="features-section" id="features">
          <div className="section-shell">
            <header className="section-heading features-heading">
              <p className="section-kicker"><span>02</span> Engineered for play</p>
              <div>
                <h2>Every frame.<br /><em>Under control.</em></h2>
                <p>framegen-vulkan’s scaler, scheduler, profiles, and launch boundaries are designed to stay fast, predictable, and out of your way.</p>
              </div>
            </header>

            <div className="feature-grid">
              {features.map((feature) => (
                <article className={`feature-card feature-${feature.tone}`} key={feature.index}>
                  <div className="feature-top"><span>{feature.index}</span><i>{feature.code}</i></div>
                  <h3>{feature.title}</h3>
                  <p>{feature.text}</p>
                  <div className="feature-stat"><strong>{feature.stat}</strong><span>{feature.label}</span></div>
                </article>
              ))}
            </div>
          </div>
        </section>

        <div className="generation-scaling-stack section-shell">
          <div className="generation-scaling-divider" aria-hidden="true"><span>FRAME GENERATION</span><i /><span>SPATIAL SCALING</span><i /><span>SHADERS</span></div>

        <section className="adaptive-section processing-section">
          <div className="adaptive-copy">
            <p className="section-kicker"><span>03</span> Adaptive intelligence</p>
            <h2>It doesn’t just add frames.<br /><em>It reads the rhythm.</em></h2>
            <p>Choose a target and ceiling. framegen-vulkan observes each game interval, plans only the generated frames needed to reach the output rhythm, and backs off cleanly when delivery becomes unstable.</p>
            <div className="adaptive-points">
              <div><span>01</span><p><strong>Observe</strong> game cadence and delivery health</p></div>
              <div><span>02</span><p><strong>Plan</strong> the right generated-frame count</p></div>
              <div><span>03</span><p><strong>Recover</strong> without sacrificing native presentation</p></div>
            </div>
          </div>

          <div className="telemetry-card processing-visual" aria-label="Adaptive frame generation example: changing native cadence with generated-frame segments filling the output to a 90 FPS target.">
            <div className="telemetry-head"><span>ADAPTIVE TELEMETRY</span></div>
            <div className="telemetry-readout">
              <div><span>TARGET</span><strong>90<small> FPS</small></strong></div>
              <div><span>CEILING</span><strong>3<small> ×</small></strong></div>
              <div><span>STATUS</span><strong className="healthy">STABLE</strong></div>
            </div>
            <div className="chart-grid">
              <div className="chart-y"><span>120</span><span>90</span><span>60</span><span>0</span></div>
              <div className="chart-area">
                <div className="chart-key" aria-hidden="true"><span><i className="native-key" />NATIVE CADENCE</span><span><i className="generated-key" />GENERATED FRAMES</span></div>
                <div className="target-line"><span>90 FPS OUTPUT</span></div>
                <div className="bars" aria-hidden="true">
                  {[31, 40, 34, 47, 38, 45, 32, 49, 42, 36, 46, 39, 50, 33, 44, 37, 48, 41].map((nativeCadence, index) => (
                    <i className="cadence-bar" key={index}>
                      <span className="native-bar" style={{ height: `${nativeCadence}%` }} />
                      <span className="generated-bar" style={{ height: `${75 - nativeCadence}%` }} />
                    </i>
                  ))}
                </div>
              </div>
            </div>
            <div className="telemetry-foot"><span>INPUT CADENCE 37-60 FPS</span><span>OUTPUT RHYTHM 90 FPS</span></div>
          </div>
        </section>

        <div className="processing-divider" aria-hidden="true" />

        <section className="scaling-section processing-section" id="scaling">
          <div className="scaling-copy">
            <p className="section-kicker"><span>04</span> Spatial scaling</p>
            <h2>Make each pixel.<br /><em>Count twice.</em></h2>
            <p>Render fewer source pixels when you need headroom, then reconstruct them for the display you actually play on. Pick the fast Native baseline, the open framegen-vulkan Scaler, or the licensed LS1 models, then run scaling by itself or ahead of frame generation.</p>
            <div className="scaling-modes">
              <div><span>NATIVE</span><strong>Fast linear baseline</strong></div>
              <div className="scaling-mode-active"><span>framegen-vulkan</span><strong>Open single-pass scaler</strong></div>
              <div><span>LS1</span><strong>Licensed quality or performance</strong></div>
            </div>
          </div>

          <div className="scaling-visual processing-visual" aria-label="Spatial scaling visual showing a 1280 by 720 render target reconstructed for a 1920 by 1080 display target.">
            <div className="scaling-visual-head"><span>SPATIAL RECONSTRUCTION</span><span>INPUT → DISPLAY</span></div>
            <div className="resolution-flow" aria-hidden="true">
              <div className="resolution-surface resolution-source">
                <span>RENDER TARGET</span><strong>1280 × 720</strong>
                <div className="resolution-preview resolution-preview-source"><img src="assets/fgvk-logo.webp" alt="" width="256" height="256" draggable={false} /></div>
                <small>COARSE INPUT SAMPLE</small>
              </div>
              <div className="scaling-transfer"><span>1.50×</span><i>→</i><small>RECONSTRUCT</small></div>
              <div className="resolution-surface resolution-output">
                <span>DISPLAY TARGET</span><strong>1920 × 1080</strong>
                <div className="resolution-preview resolution-preview-output"><img src="assets/fgvk-logo.webp" alt="" width="256" height="256" draggable={false} /></div>
                <small>RECONSTRUCTED DETAIL</small>
              </div>
            </div>
            <div className="scaling-status"><span><i /> framegen-vulkan Scaler READY</span><span>1.50× SHOWN <b>·</b> UP TO 2.00×</span></div>
          </div>
        </section>

        <div className="processing-divider" aria-hidden="true" />

        <section className="shader-section processing-section" id="shaders">
          <div className="shader-copy">
            <p className="section-kicker"><span>05</span> Shaders</p>
            <h2>Sharpen. Smooth.<br /><em>Stylize.</em></h2>
            <p>Use per-game shaders to make details crisper, soften jagged edges, or shift the colour and mood. Compare the image before and after, then tune supported effects live while you play.</p>
            <div className="shader-modes">
              <div><span>DETAIL</span><strong>CAS or DLS sharpening</strong></div>
              <div><span>EDGES</span><strong>FXAA or SMAA smoothing</strong></div>
              <div className="shader-mode-active"><span>STYLE</span><strong>Seven finishing presets</strong></div>
            </div>
          </div>

          <div className="shader-visual processing-visual" aria-label="Per-game shader preview with live sharpening, anti-aliasing, and colour controls.">
            <div className="shader-visual-head"><span>SHADER PIPELINE</span><span>PER-GAME · LIVE</span></div>
            <div className="shader-stage">
              <div className="shader-preview" aria-hidden="true">
                <span className="shader-before-label">BEFORE</span>
                <span className="shader-after-label">SHADER</span>
                <img className="shader-before-image" src="assets/fgvk-logo.webp" alt="" width="256" height="256" draggable={false} />
                <img className="shader-after-image" src="assets/fgvk-logo.webp" alt="" width="256" height="256" draggable={false} />
                <i className="shader-split"><b>↔</b></i>
                <small>MOVE FROM NEUTRAL TO CRISP, SMOOTH, OR STYLIZED</small>
              </div>
              <div className="shader-controls" aria-hidden="true">
                <div><span>SHARPENING</span><strong>DLS</strong><i><b style={{ width: '64%' }} /></i></div>
                <div><span>ANTI-ALIASING</span><strong>SMAA</strong><i><b style={{ width: '48%' }} /></i></div>
                <div><span>COLOUR</span><strong>VIBRANCE</strong><i><b style={{ width: '72%' }} /></i></div>
              </div>
            </div>
            <div className="shader-status"><span><i /> EFFECTS ACTIVE</span><span>LIVE TUNING</span></div>
          </div>
        </section>
        </div>

        <section className="start-section">
          <div className="section-shell start-inner">
            <header className="start-heading">
              <p className="section-kicker"><span>06</span> From zero to smoother</p>
              <h2>Three steps.<br /><em>Then dive in.</em></h2>
            </header>
            <ol className="steps">
              <li>
                <span>01</span>
                <div><strong>Choose your pipeline</strong><p>The open framegen-vulkan Scaler and bundled shaders run independently. For frame generation or LS1 scaling, purchase and install Lossless Scaling separately through Steam so framegen-vulkan can read your licensed <code>Lossless.dll</code>.</p></div>
              </li>
              <li>
                <span>02</span>
                <div><strong>Install framegen-vulkan</strong><p>Extract the framegen-vulkan archive and run its installer, or install the Arch package on Arch Linux.</p></div>
              </li>
              <li>
                <span>03</span>
                <div><strong>Launch your game</strong><p>Start with Fixed 2×, then tune one setting at a time or move to an Adaptive FPS target.</p></div>
              </li>
            </ol>
            <a className="text-link" href={links.docs} {...newTabProps}>Read the installation guide <span>→</span></a>
          </div>
        </section>

        <section className="downloads-section section-shell" id="downloads">
          <header className="downloads-heading">
            <p className="section-kicker"><span>07</span> Latest release</p>
            <h2>Choose your<br /><em>entry point.</em></h2>
            <p>framegen-vulkan is open source. The current package is framegen-vulkan v{rendererVersion}.</p>
          </header>

          <div className="download-grid">
            <article className="download-card download-renderer">
              <div className="download-badge">DIRECT LINUX INSTALL</div>
              <div className="download-icon"><img src="assets/fgvk-render-logo.webp" alt="" width="88" height="88" draggable={false} /></div>
              <p className="download-type">STANDALONE VULKAN LAYER</p>
              <h3>framegen-vulkan</h3>
              <p className="download-copy">The direct host archive for desktop Linux, with the launcher, CLI, both x86 Vulkan layers, and bundled shader effects.</p>
              <dl>
                <div><dt>VERSION</dt><dd>{rendererVersion}</dd></div>
                <div><dt>FORMAT</dt><dd>TAR.XZ</dd></div>
                <div><dt>ARCH</dt><dd>x86_64</dd></div>
              </dl>
              <a className="download-button" href={links.rendererDownload} {...newTabProps}><span>Download Renderer</span><i>→</i></a>
              <div className="release-links">
                <a className="release-link" href={links.flatpakDownload} {...newTabProps}>Flatpak bundle <span>→</span></a>
                <a className="release-link" href={links.rendererRelease} {...newTabProps}>Release notes <span>→</span></a>
              </div>
            </article>
          </div>

          <div className="requirement-note">
            <span>IMPORTANT / LICENSED INPUT</span>
            <p>framegen-vulkan is independent and unaffiliated with Lossless Scaling. Frame generation and LS1 require your own separately purchased <a href={links.losslessScaling} {...newTabProps}>Lossless Scaling</a> installation; the open framegen-vulkan Scaler does not. framegen-vulkan does not distribute Lossless Scaling or its proprietary resources, alter your DLL, or save extracted resources as standalone files. Your copy’s terms apply. See <a href={`${links.repository}/blob/main/THIRD_PARTY_NOTICES.md`} {...newTabProps}>third-party notices</a>.</p>
          </div>
        </section>

        <section className="faq-section section-shell" id="faq">
          <header>
            <p className="section-kicker"><span>08</span> Signal check</p>
            <h2>Before you<br /><em>make the jump.</em></h2>
          </header>
          <div className="faq-list">
            <details>
              <summary><span>01</span> Which version should I install?<i>+</i></summary>
              <p>Use the framegen-vulkan host archive for SteamOS and desktop Linux, or the Arch package on Arch Linux. Use the Flatpak bundle for Flatpak games, launchers, and emulators.</p>
            </details>
            <details>
              <summary><span>02</span> Does framegen-vulkan include Lossless Scaling?<i>+</i></summary>
              <p>No. framegen-vulkan does not contain or distribute Lossless Scaling, <code>Lossless.dll</code>, or extracted proprietary model payloads. Frame generation and LS1 scaling require a separate Lossless Scaling purchase and installation from Steam, then framegen-vulkan discovers or lets you select its local DLL. The open framegen-vulkan Scaler does not need it.</p>
            </details>
            <details>
              <summary><span>03</span> Can frame generation, scaling, and shaders run together?<i>+</i></summary>
              <p>Yes. Scale with LS1 or the open framegen-vulkan Scaler, feed reconstructed real frames into Fixed or Adaptive frame generation, and apply per-game shader effects in the same profile. The downloads above track framegen-vulkan v{rendererVersion} from canonical release metadata.</p>
            </details>
            <details>
              <summary><span>04</span> What hardware is published today?<i>+</i></summary>
              <p>framegen-vulkan v{rendererVersion} targets x86_64 Linux hosts and includes 64-bit and 32-bit x86 Vulkan layers. Native AArch64 packages are not included in this release.</p>
            </details>
          </div>
        </section>

        <section className="final-cta section-shell">
          <div className="cta-glow" />
          <img src="assets/fgvk-logo.webp" alt="framegen-vulkan shark" width="260" height="260" draggable={false} />
          <div>
            <p className="eyebrow"><span /> Motion-Adaptive Kernel Orchestration</p>
            <h2>Play smoother.<br /><em>Look sharper.</em></h2>
            <p>Download framegen-vulkan for SteamOS, Steam Machine, or desktop Linux.</p>
            <div className="hero-actions">
              <a className="button button-primary" href={links.rendererDownload} {...newTabProps}>Download framegen-vulkan <span>→</span></a>
              <a className="button button-secondary social-button" href={links.repository} {...newTabProps}><span className="social-button-label"><GitHubIcon />View on GitHub</span><span aria-hidden="true">→</span></a>
              <a className="button button-secondary social-button" href={links.discord} {...newTabProps}><span className="social-button-label"><DiscordIcon />Join Discord</span><span aria-hidden="true">→</span></a>
            </div>
          </div>
        </section>
      </div>

      <footer className="site-footer">
        <div className="footer-brand">
          <a className="brand" href="#top"><img src="assets/fgvk-discord-icon.webp" alt="" width="42" height="42" draggable={false} /><span>framegen-vulkan</span></a>
          <p>Frame generation, spatial scaling, and bundled shaders for Steam Deck, Steam Machine, SteamOS, and Linux.</p>
        </div>
        <div className="footer-links">
          <div><span>PROJECT</span><a className="social-link" href={links.repository} {...newTabProps}><GitHubIcon /><span>GitHub</span></a><a className="social-link" href={links.discord} {...newTabProps}><DiscordIcon /><span>Discord</span></a><a href={links.docs} {...newTabProps}>Installation Guide</a><a href="third-party-notices.txt">Third-party notices</a><a href={links.issues} {...newTabProps}>Issues</a></div>
          <div><span>DOWNLOAD</span><a href={links.rendererRelease} {...newTabProps}>framegen-vulkan</a><a href={links.allReleases} {...newTabProps}>All releases</a></div>
        </div>
        <div className="footer-bottom"><span>GPL-3.0-OR-LATER</span><span>INDEPENDENT COMMUNITY PROJECT</span><span>© 2026 framegen-vulkan</span></div>
      </footer>
    </main>
  );
}
