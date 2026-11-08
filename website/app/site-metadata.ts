import { releaseData } from './release-data';

export const siteUrl = 'https://eugeniosegala.github.io/MAKO/';
export const siteTitle =
  'framegen-vulkan: Frame Generation, Scaling, and Shaders for SteamOS and Linux';
export const siteDescription =
  'framegen-vulkan brings frame generation, spatial scaling, and per-game shader effects to Steam Deck, SteamOS, and desktop Linux through Vulkan.';
export const socialImageUrl = `${siteUrl}og.png`;
export const siteKeywords = [
  'framegen-vulkan',
  'Steam Deck frame generation',
  'Linux frame generation',
  'SteamOS frame generation',
  'Linux spatial scaling',
  'Steam Deck shaders',
  'Vulkan frame generation',
  'Lossless Scaling Linux',
];

export const structuredData = {
  '@context': 'https://schema.org',
  '@graph': [
    {
      '@type': 'WebSite',
      '@id': `${siteUrl}#website`,
      url: siteUrl,
      name: 'framegen-vulkan',
      alternateName: 'framegen-vulkan',
      description: siteDescription,
      inLanguage: 'en',
    },
    {
      '@type': 'SoftwareApplication',
      '@id': `${siteUrl}#software`,
      name: 'framegen-vulkan',
      alternateName: 'framegen-vulkan',
      url: siteUrl,
      description: siteDescription,
      applicationCategory: 'UtilitiesApplication',
      operatingSystem: 'SteamOS, Linux',
      isAccessibleForFree: true,
      offers: {
        '@type': 'Offer',
        price: '0',
        priceCurrency: 'USD',
        availability: 'https://schema.org/InStock',
      },
      softwareVersion: `framegen-vulkan ${releaseData.rendererVersion}`,
      downloadUrl: releaseData.links.rendererDownload,
      codeRepository: releaseData.links.repository,
      license: `${releaseData.links.repository}/blob/main/LICENSE.md`,
      image: socialImageUrl,
      softwareRequirements:
        'Lossless Scaling is required only for LSFG frame generation and LS1 scaling. framegen-vulkan Scaler and bundled shaders work independently.',
      featureList: [
        'Fixed and adaptive frame generation',
        'Spatial scaling with framegen-vulkan Scaler or LS1',
        'Per-game Vulkan shader effects',
        'Steam Deck and SteamOS launch integration through a Steam launch option',
        'Standalone framegen-vulkan for desktop Linux',
      ],
      mainEntityOfPage: { '@id': `${siteUrl}#website` },
    },
    {
      '@type': 'FAQPage',
      '@id': `${siteUrl}#faq`,
      mainEntity: [
        {
          '@type': 'Question',
          name: 'Which framegen-vulkan version should I install?',
          acceptedAnswer: {
            '@type': 'Answer',
            text: 'Use the framegen-vulkan host archive for SteamOS and desktop Linux, or the Arch package on Arch Linux.',
          },
        },
        {
          '@type': 'Question',
          name: 'Does framegen-vulkan include Lossless Scaling?',
          acceptedAnswer: {
            '@type': 'Answer',
            text: 'No. framegen-vulkan is independent and does not distribute Lossless Scaling or its proprietary resources. Frame generation and LS1 scaling require a separate Lossless Scaling purchase. framegen-vulkan Scaler and the bundled shaders do not.',
          },
        },
        {
          '@type': 'Question',
          name: 'Can frame generation, scaling, and shaders run together?',
          acceptedAnswer: {
            '@type': 'Answer',
            text: 'Yes. A framegen-vulkan profile can combine spatial scaling, fixed or adaptive frame generation, and per-game shader effects.',
          },
        },
        {
          '@type': 'Question',
          name: 'Which operating systems and architectures does framegen-vulkan support?',
          acceptedAnswer: {
            '@type': 'Answer',
            text: 'framegen-vulkan targets SteamOS and desktop Linux on x86_64 hosts. Published Renderer packages include both 64-bit and 32-bit x86 Vulkan layers.',
          },
        },
      ],
    },
  ],
} as const;

export const structuredDataJson = JSON.stringify(structuredData).replaceAll(
  '<',
  '\\u003c',
);
