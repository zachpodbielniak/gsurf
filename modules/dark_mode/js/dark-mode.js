/*
 * dark-mode.js - gsurf dark_mode page engine
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Injected by the dark_mode module at document start into every frame,
 * inside the isolated script world "gsurf-dark-mode": it shares the DOM
 * with the page but none of its JavaScript globals, so the page can
 * neither read the configuration nor call these functions.
 *
 * The module appends `__gsurf_dark.init(<state>)` to this file. Every
 * later runtime change (a toggle, a reconfigure) is delivered twice: as
 * an extra document-start user script `__gsurf_dark.set_state(<state>)`
 * (so documents loaded later in an existing view start from the current
 * state, synchronously, without a light flash) and evaluated once in the
 * current main frame (so the open page changes at once).
 *
 * State (built by the module, see docs/dark-mode.org):
 *   { version, enabled, fallback,
 *     global:  { mode, scheme, brightness, contrast, sepia, grayscale },
 *     rules:   [ { pattern, mode?, scheme?, brightness?, ... } ],
 *     overrides: { "<host>": true|false } }
 *
 * The decision is made for the top-level page's host; subframes follow
 * it. decide() is a pure function so tests can exercise it directly.
 */
(function () {
	'use strict';

	/* A second evaluation in the same world (should not happen) keeps the
	 * first engine and its applied state. */
	if (globalThis.__gsurf_dark !== undefined)
		return;

	var MESSAGE = 'gsurf_dark_mode';
	var ADJUST = ['brightness', 'contrast', 'sepia', 'grayscale'];
	var NEUTRAL = { brightness: 100, contrast: 100, sepia: 0, grayscale: 0 };
	/* Elements re-inverted in filter mode so photos and video keep their
	 * colours (picture is left out: its img is already re-inverted). */
	var MEDIA = 'img,video,canvas,embed,object';
	var DARK_QUERY = /\(\s*prefers-color-scheme\s*:\s*dark\s*\)/gi;
	var LIGHT_QUERY = /\(\s*prefers-color-scheme\s*:\s*light\s*\)/gi;
	/* Shown until Dark Reader arrives when it is loaded on demand. */
	var PLACEHOLDER = 'html,body,body :not(iframe){background-color:#181a1b!important;' +
		'border-color:#776e62!important;color:#e8e6e3!important;}';

	var engine = {
		state: null,        /* the last state received */
		current: null,      /* the decision applied to this document */
		is_top: true,
		host: '',
		style: null,        /* our <style> element */
		requested: false,   /* need-engine already posted */
		dr_on: false,       /* DarkReader.enable() in force */
		fetch_set: false,
		rewrite_on: false,  /* native: prefers-color-scheme rewriting */
		saved: [],          /* [object, kind, original] to restore */
		fixed: new WeakSet(),
		observer: null,
		scan_pending: false
	};

	/* --- Pure decision logic ------------------------------------------ */

	function normalize_host(host) {
		host = String(host || '').toLowerCase();
		if (host.charAt(0) === '[' && host.charAt(host.length - 1) === ']')
			host = host.slice(1, -1);
		while (host.length > 0 && host.charAt(host.length - 1) === '.')
			host = host.slice(0, -1);
		return host;
	}

	/* A mode that actually darkens the page (legacy "auto" only follows
	 * the system preference, so it does not count). */
	function is_dark(mode, scheme) {
		return mode !== undefined && mode !== 'off' &&
			!(mode === 'native' && scheme === 'light dark');
	}

	/*
	 * decide(state, host) -> decision
	 *
	 * Precedence, highest first: the global on/off switch
	 * (state.enabled), the per-site override from the state file
	 * (state.overrides[host]), the first config rule whose pattern
	 * matches host, the global settings. A matching rule replaces only
	 * the keys it sets; the rest come from the global settings. An
	 * override of true on a host whose configured mode is not dark uses
	 * the rule's mode, else the global mode, else state.fallback.
	 */
	function decide(state, host) {
		var g = (state && state.global) || {};
		var rules = (state && state.rules) || [];
		var overrides = (state && state.overrides) || {};
		var out = {
			mode: g.mode || 'off',
			scheme: g.scheme || 'dark',
			source: 'global',
			rule: -1,
			host: normalize_host(host)
		};
		var rule = null;
		var i, re, ov;

		ADJUST.forEach(function (key) {
			out[key] = typeof g[key] === 'number' ? g[key] : NEUTRAL[key];
		});

		/* First matching rule wins; an invalid pattern never matches. */
		if (out.host !== '') {
			for (i = 0; i < rules.length && rule === null; i++) {
				try {
					re = new RegExp(rules[i].pattern, 'i');
				} catch (e) {
					continue;
				}
				if (re.test(out.host)) {
					rule = rules[i];
					out.rule = i;
					out.source = 'rule';
				}
			}
		}
		if (rule !== null) {
			if (typeof rule.mode === 'string') {
				out.mode = rule.mode;
				out.scheme = rule.scheme || 'dark';
			}
			ADJUST.forEach(function (key) {
				if (typeof rule[key] === 'number')
					out[key] = rule[key];
			});
		}

		/* The runtime per-site override (state file). */
		ov = out.host !== '' && Object.prototype.hasOwnProperty.call(overrides, out.host)
			? overrides[out.host] : undefined;
		if (ov === false) {
			out.mode = 'off';
			out.source = 'override';
		} else if (ov === true) {
			out.source = 'override';
			if (!is_dark(out.mode, out.scheme)) {
				if (rule !== null && is_dark(rule.mode, rule.scheme)) {
					out.mode = rule.mode;
					out.scheme = rule.scheme || 'dark';
				} else if (is_dark(g.mode, g.scheme)) {
					out.mode = g.mode;
					out.scheme = g.scheme || 'dark';
				} else {
					out.mode = (state && state.fallback) || 'filter';
					out.scheme = 'dark';
				}
			}
		}

		/* The global switch beats everything. */
		if (state && state.enabled === false) {
			out.mode = 'off';
			out.source = 'disabled';
		}
		return out;
	}

	/* --- Page plumbing ------------------------------------------------ */

	/* Host of the top-level page: this frame's own when it is the top,
	 * else the top's (same origin), else the outermost ancestor origin,
	 * else the referrer, else our own host. */
	function top_host() {
		var origins, url;

		if (engine.is_top)
			return location.hostname;
		try {
			return window.top.location.hostname;
		} catch (e) { /* cross-origin top */ }
		origins = location.ancestorOrigins;
		if (origins && origins.length > 0) {
			try {
				url = new URL(origins[origins.length - 1]);
				return url.hostname;
			} catch (e) { /* opaque origin */ }
		}
		if (document.referrer) {
			try {
				return new URL(document.referrer).hostname;
			} catch (e) { /* not a URL */ }
		}
		return location.hostname;
	}

	function post(message) {
		try {
			window.webkit.messageHandlers[MESSAGE].postMessage(message);
		} catch (e) { /* handler not registered (tests, other hosts) */ }
	}

	function percent(value) {
		return String(value / 100);
	}

	/* brightness() ... grayscale() for the non-neutral adjustments. */
	function adjust_filters(d) {
		var parts = [];

		ADJUST.forEach(function (key) {
			if (d[key] !== NEUTRAL[key])
				parts.push(key + '(' + percent(d[key]) + ')');
		});
		return parts;
	}

	function engine_loaded() {
		return typeof globalThis.DarkReader === 'object' && globalThis.DarkReader !== null;
	}

	/* The stylesheet text for decision @d in this frame ('' = none). */
	function build_css(d) {
		var css = '';
		var filters;

		switch (d.mode) {
		case 'filter':
			if (engine.is_top) {
				/* The top frame's filter also covers every subframe's
				 * pixels, so subframes only re-invert their media. */
				filters = ['invert(1)', 'hue-rotate(180deg)'].concat(adjust_filters(d));
				css += 'html{filter:' + filters.join(' ') + '!important;}';
			}
			css += MEDIA + '{filter:invert(1) hue-rotate(180deg)!important;}';
			break;
		case 'native':
			css += d.scheme === 'light dark'
				? ':root{color-scheme:light dark;}'
				: ':root{color-scheme:dark!important;}';
			filters = adjust_filters(d);
			if (engine.is_top && filters.length > 0)
				css += 'html{filter:' + filters.join(' ') + '!important;}';
			break;
		case 'dynamic':
			if (!engine_loaded() && engine.is_top)
				css += PLACEHOLDER;
			break;
		default:
			break;
		}
		return css;
	}

	/* Keep our <style> as the first child of <html> (page styles come
	 * after it; the declarations that must win carry !important). */
	function attach_style() {
		var root = document.documentElement;

		if (engine.style === null || root === null || engine.style.isConnected)
			return;
		root.insertBefore(engine.style, root.firstChild);
	}

	function set_style(css) {
		if (css === '') {
			if (engine.style !== null)
				engine.style.remove();
			engine.style = null;
			return;
		}
		if (engine.style === null)
			engine.style = document.createElement('style');
		if (engine.style.textContent !== css)
			engine.style.textContent = css;
		attach_style();
	}

	/* --- native: prefers-color-scheme rewriting ----------------------- */

	function rewrite_query(text) {
		return text.replace(DARK_QUERY, '(min-width: 0px)')
			.replace(LIGHT_QUERY, '(max-width: 0px) and (min-width: 1px)');
	}

	function fix_media_list(list) {
		var text, next;

		if (!list || engine.fixed.has(list))
			return;
		text = list.mediaText;
		next = rewrite_query(text);
		if (next === text)
			return;
		engine.fixed.add(list);
		engine.saved.push([list, 'media', text]);
		list.mediaText = next;
	}

	function fix_rules(rules) {
		var i, rule;

		for (i = 0; i < rules.length; i++) {
			rule = rules[i];
			if (rule.media !== undefined)
				fix_media_list(rule.media);
			if (rule.styleSheet)   /* @import */
				fix_sheet(rule.styleSheet);
			else if (rule.cssRules)   /* @media, @supports, @layer, ... */
				fix_rules(rule.cssRules);
		}
	}

	function fix_sheet(sheet) {
		var rules;

		fix_media_list(sheet.media);
		try {
			rules = sheet.cssRules;
		} catch (e) {
			return;   /* cross-origin: the CSSOM is not readable */
		}
		if (rules)
			fix_rules(rules);
	}

	/* Rewrite every reachable stylesheet and <source media>. */
	function scan() {
		var i, sources, el, text, next;

		engine.scan_pending = false;
		if (!engine.rewrite_on)
			return;
		for (i = 0; i < document.styleSheets.length; i++)
			fix_sheet(document.styleSheets[i]);
		sources = document.querySelectorAll('source[media]');
		for (i = 0; i < sources.length; i++) {
			el = sources[i];
			if (engine.fixed.has(el))
				continue;
			text = el.getAttribute('media');
			next = rewrite_query(text);
			if (next !== text) {
				engine.fixed.add(el);
				engine.saved.push([el, 'attr', text]);
				el.setAttribute('media', next);
			}
		}
	}

	function schedule_scan() {
		if (engine.scan_pending)
			return;
		engine.scan_pending = true;
		setTimeout(scan, 0);
	}

	function on_load_capture(event) {
		var t = event.target;

		if (t && (t.tagName === 'LINK' || t.tagName === 'STYLE'))
			schedule_scan();
	}

	function rewrite_start() {
		if (engine.rewrite_on)
			return;
		engine.rewrite_on = true;
		engine.observer = new MutationObserver(function (records) {
			var i, j, node;

			for (i = 0; i < records.length; i++) {
				for (j = 0; j < records[i].addedNodes.length; j++) {
					node = records[i].addedNodes[j];
					if (node.nodeType === 1 && /^(STYLE|LINK|SOURCE|PICTURE)$/.test(node.tagName)) {
						schedule_scan();
						return;
					}
				}
			}
		});
		engine.observer.observe(document, { childList: true, subtree: true });
		document.addEventListener('load', on_load_capture, true);
		document.addEventListener('DOMContentLoaded', schedule_scan);
		scan();
	}

	function rewrite_stop() {
		var i, item;

		if (!engine.rewrite_on)
			return;
		engine.rewrite_on = false;
		if (engine.observer !== null)
			engine.observer.disconnect();
		engine.observer = null;
		document.removeEventListener('load', on_load_capture, true);
		document.removeEventListener('DOMContentLoaded', schedule_scan);
		for (i = engine.saved.length - 1; i >= 0; i--) {
			item = engine.saved[i];
			try {
				if (item[1] === 'media')
					item[0].mediaText = item[2];
				else
					item[0].setAttribute('media', item[2]);
			} catch (e) { /* the sheet went away */ }
		}
		engine.saved = [];
		engine.fixed = new WeakSet();
	}

	/* --- dynamic: the vendored Dark Reader ----------------------------- */

	function dark_reader_on(d) {
		if (!engine_loaded()) {
			/* Not preloaded: ask the module to load it into the main
			 * frame; subframes stay as they are. */
			if (engine.is_top && !engine.requested) {
				engine.requested = true;
				post({ type: 'need-engine', host: engine.host });
			}
			return;
		}
		if (!engine.fetch_set) {
			/* Lets Dark Reader read cross-origin stylesheets the page's
			 * CORS policy allows. */
			globalThis.DarkReader.setFetchMethod(window.fetch.bind(window));
			engine.fetch_set = true;
		}
		globalThis.DarkReader.enable({
			brightness: d.brightness,
			contrast: d.contrast,
			sepia: d.sepia,
			grayscale: d.grayscale
		});
		engine.dr_on = true;
	}

	function dark_reader_off() {
		if (engine.dr_on && engine_loaded())
			globalThis.DarkReader.disable();
		engine.dr_on = false;
	}

	/* --- Applying a decision ------------------------------------------ */

	function apply(d) {
		set_style(build_css(d));
		if (d.mode === 'native' && d.scheme === 'dark')
			rewrite_start();
		else
			rewrite_stop();
		if (d.mode === 'dynamic')
			dark_reader_on(d);
		else
			dark_reader_off();
		engine.current = d;
	}

	function refresh() {
		var d;

		engine.host = normalize_host(top_host());
		d = decide(engine.state, engine.host);
		apply(d);
		return d;
	}

	/* Re-attach the style if the parser or the page replaced <html>'s
	 * children before the document finished loading. */
	function on_ready() {
		attach_style();
		if (engine.rewrite_on)
			schedule_scan();
	}

	globalThis.__gsurf_dark = Object.freeze({
		decide: decide,
		normalize_host: normalize_host,
		/* First state, from the script baked at view creation. */
		init: function (state) {
			try {
				engine.is_top = window === window.top;
			} catch (e) {
				engine.is_top = false;
			}
			engine.state = state;
			document.addEventListener('DOMContentLoaded', on_ready);
			return refresh();
		},
		/* A newer state (toggle, reconfigure); a state older than the
		 * one in force is ignored. */
		set_state: function (state) {
			if (engine.state !== null && state && engine.state.version > state.version)
				return engine.current;
			engine.state = state;
			return refresh();
		},
		/* Called after the module loaded Dark Reader on demand. */
		engine_ready: function () {
			return refresh();
		},
		/* Introspection for tests. */
		get current() {
			return engine.current;
		},
		get state() {
			return engine.state;
		},
		get is_top() {
			return engine.is_top;
		}
	});
})();
