// @ts-check
// help.compassbrowser.org: Compass's help center (Astro Starlight).
// Pages are Markdown in src/content/docs/<section>/; each section's sidebar
// is generated from its folder, ordered by each page's `sidebar.order`.
// See README.md for building and publishing.
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';

export default defineConfig({
	site: 'https://help.compassbrowser.org',
	integrations: [
		starlight({
			title: 'Compass Help',
			description:
				'How to use Compass Browser on Mac, Windows, Linux, iPhone, iPad and Android, and how to run your own Compass sync server.',
			logo: { src: './src/assets/compass.svg', alt: 'Compass' },
			favicon: '/favicon.svg',
			head: [
				{ tag: 'link', attrs: { rel: 'icon', href: '/favicon.ico', sizes: '16x16 32x32 48x48' } },
			],
			social: [
				{ icon: 'github', label: 'GitHub', href: 'https://github.com/Compass-Browser/Compass' },
			],
			editLink: {
				baseUrl: 'https://github.com/Compass-Browser/Compass/edit/main/help/',
			},
			lastUpdated: true,
			customCss: ['./src/styles/compass.css'],
			sidebar: [
				{ label: 'Get started', items: [{ autogenerate: { directory: 'start' } }] },
				{ label: 'Using Compass', items: [{ autogenerate: { directory: 'guide' } }] },
				{ label: 'Sync', items: [{ autogenerate: { directory: 'sync' } }] },
				{ label: 'Run your own sync server', items: [{ autogenerate: { directory: 'self-hosting' } }] },
			],
		}),
	],
});
