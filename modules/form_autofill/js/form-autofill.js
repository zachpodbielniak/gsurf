/*
 * form-autofill.js - page side of the gsurf form_autofill module
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Runs in the isolated script world "gsurf-autofill", so the page's own
 * JavaScript cannot see these globals, call these functions or read the
 * suggestion dropdown (a closed shadow root). It holds NO profile values:
 * the host only sends the labels to list (arm()) and, after the user
 * picked one, the values for the focused form (fill()). Those values are
 * written into the form fields and dropped.
 *
 * Layout:
 *   1. the classifier core: pure functions over plain field descriptors,
 *      exported as globalThis.__gsurfAutofillCore so the headless test
 *      suite can evaluate it in a bare JavaScriptCore context;
 *   2. the DOM side (top-level document only): field discovery, the
 *      suggestion dropdown, keyboard/mouse handling, the fill itself and
 *      the messages to the host (window.webkit.messageHandlers).
 */
(function () {
	'use strict';

	/* ================= 1. classifier core (DOM-free) ================= */

	/* autocomplete field names we fill, mapped to our field types. */
	var AC_FIELDS = {
		'name': 'name',
		'given-name': 'given_name',
		'family-name': 'family_name',
		'organization': 'organization',
		'street-address': 'street',
		'address-line1': 'address_line1',
		'address-line2': 'address_line2',
		'address-line3': 'address_line3',
		'address-level2': 'city',
		'address-level1': 'region',
		'postal-code': 'postal_code',
		'country': 'country',
		'country-name': 'country_name',
		'email': 'email',
		'tel': 'tel',
		'tel-national': 'tel',
		'cc-name': 'cc_name',
		'cc-given-name': 'cc_given_name',
		'cc-family-name': 'cc_family_name',
		'cc-number': 'cc_number',
		'cc-exp': 'cc_exp',
		'cc-exp-month': 'cc_exp_month',
		'cc-exp-year': 'cc_exp_year',
		'cc-csc': 'cc_csc'
	};

	/* Valid autofill field names we deliberately never fill. Naming one is
	 * a definite answer: heuristics must not second-guess it (a field the
	 * page marked "username" or "one-time-code" is never an address). */
	var AC_IGNORED = [
		'additional-name', 'honorific-prefix', 'honorific-suffix', 'nickname',
		'username', 'new-password', 'current-password', 'one-time-code',
		'organization-title', 'address-level3', 'address-level4', 'cc-type',
		'cc-additional-name', 'transaction-currency', 'transaction-amount',
		'language', 'bday', 'bday-day', 'bday-month', 'bday-year', 'sex',
		'url', 'photo', 'impp', 'tel-country-code', 'tel-area-code',
		'tel-local', 'tel-local-prefix', 'tel-local-suffix', 'tel-extension'
	];

	/* Contact-type tokens that may precede tel/email; they carry no
	 * meaning for us. */
	var AC_CONTACT = ['home', 'work', 'mobile', 'fax', 'pager'];

	/* Input types that never take profile data. */
	var SKIP_TYPES = [
		'hidden', 'password', 'submit', 'button', 'reset', 'checkbox',
		'radio', 'file', 'image', 'range', 'color', 'date', 'datetime-local',
		'time', 'week', 'search', 'url'
	];

	/* A source that matches this is not ours at all (login, search,
	 * coupon boxes): classification stops with "no field". */
	var NEGATIVE = /(user ?name|\blogin\b|log ?in|passw|captcha|search|coupon|promo|voucher|discount|gift ?card|referr?al|\botp\b)/;

	/* Heuristic table: first matching rule wins. Card rules come first so
	 * "card holder name" is cc_name and not name; month/year rules come
	 * before the general expiry rule; the bare "name" rule is last. Each
	 * source string is normalized first (see normalize()). */
	var HEURISTICS = [
		['cc_name', /(card ?holder|name ?on ?(the ?)?card|\bcc ?name|card ?name|holder ?name)/],
		['cc_csc', /(\bcvc|\bcvv|\bcsc\b|\bcid\b|security ?code|card ?verification|verification ?(value|number))/],
		['cc_exp', /(mm ?[\/-] ?yy)/],
		['cc_exp_month', /(\bexp\w* ?(date ?)?mon(th)?|\bcc ?mon(th)?|card ?mon(th)?|^mm$)/],
		['cc_exp_year', /(\bexp\w* ?(date ?)?y(ea)?r|\bcc ?y(ea)?r|card ?y(ea)?r|^yy(yy)?$)/],
		['cc_exp', /(exp(iry|iration)? ?date|\bcc ?exp|card ?exp|valid ?thru|^exp(iry|iration)?$)/],
		['cc_number', /(card ?(number|num|no\b)|\bcc ?(number|num|no\b)|\bccnum|credit ?card|debit ?card|\bpan\b)/],
		['email', /e ?mail/],
		['tel', /(phone|\btel\b|telephone|\bmobile\b|\bcell\b)/],
		['organization', /(company|organi[sz]ation|business ?name|\borg\b|employer)/],
		['given_name', /(first ?name|given ?name|\bfname\b|forename|^first$)/],
		['family_name', /(last ?name|family ?name|surname|\blname\b|^last$)/],
		['address_line2', /(address ?(line)? ?2|\baddr ?2|street ?2|\bapt\b|apartment|\bsuite\b|\bunit\b|\bline ?2)/],
		['address_line3', /(address ?(line)? ?3|\baddr ?3|\bline ?3)/],
		['address_line1', /(address ?(line)? ?1|\baddr ?1|street|\bline ?1|\baddress\b|^addr$)/],
		['postal_code', /(\bzip|postal|post ?code|\bpin ?code)/],
		['city', /(\bcity\b|\btown\b|locality|suburb)/],
		['region', /(\bstate\b|province|region|county|prefecture)/],
		['country', /(country|nation)/],
		['name', /(full ?name|your ?name|\bname\b)/]
	];

	/* Descriptor sources tried in order; the first one that says
	 * anything (a rule or a negative) decides. */
	var SOURCES = ['name', 'id', 'label', 'placeholder', 'aria'];

	/* "billing_firstName" -> "billing first name": split camelCase and
	 * treat punctuation as spaces so \b and " ?" in the table work. */
	function normalize(s) {
		if (typeof s !== 'string')
			return '';
		return s.replace(/([a-z])([A-Z])/g, '$1 $2')
			.replace(/[_\-.\[\]:()*,;#]+/g, ' ')
			.replace(/\s+/g, ' ')
			.trim()
			.toLowerCase();
	}

	/* Parse an autocomplete attribute. Returns {field, group} where field
	 * is a type, null (a valid token we never fill) or undefined (no
	 * usable hint: fall back to heuristics). group is the
	 * "section-x|shipping" part, or '' when there is none. */
	function parseAutocomplete(value) {
		var tokens, field, section = '', mode = '';

		if (typeof value !== 'string')
			return { field: undefined, group: '' };
		tokens = value.toLowerCase().trim().split(/\s+/).filter(function (t) { return t !== ''; });
		if (tokens.length === 0 || tokens[0] === 'on' || tokens[0] === 'off')
			return { field: undefined, group: '' };
		if (tokens[tokens.length - 1] === 'webauthn')
			tokens.pop();
		field = tokens.pop();
		if (tokens.length > 0 && AC_CONTACT.indexOf(tokens[tokens.length - 1]) >= 0)
			tokens.pop();
		if (tokens.length > 0 && (tokens[tokens.length - 1] === 'shipping' ||
		    tokens[tokens.length - 1] === 'billing'))
			mode = tokens.pop();
		if (tokens.length > 0 && tokens[tokens.length - 1].indexOf('section-') === 0)
			section = tokens.pop();
		/* Leftover tokens make the attribute invalid: ignore it. */
		if (tokens.length > 0 || field === undefined)
			return { field: undefined, group: '' };
		var group = (section !== '' || mode !== '') ? section + '|' + mode : '';
		if (Object.prototype.hasOwnProperty.call(AC_FIELDS, field))
			return { field: AC_FIELDS[field], group: group };
		if (AC_IGNORED.indexOf(field) >= 0)
			return { field: null, group: group };
		return { field: undefined, group: '' };
	}

	/* Classify a field descriptor {tag, type, autocomplete, name, id,
	 * label, placeholder, aria}. Returns {field, group, via}: field is a
	 * type string or null; via says which rule decided ("autocomplete",
	 * "heuristic:<source>", "type" or "none"). */
	function classify(desc) {
		var tag = String(desc.tag || '').toLowerCase();
		var type = String(desc.type || 'text').toLowerCase();
		var ac, i, j, s, field;

		if (tag !== 'input' && tag !== 'select' && tag !== 'textarea')
			return { field: null, group: '', via: 'none' };
		if (tag === 'input' && SKIP_TYPES.indexOf(type) >= 0)
			return { field: null, group: '', via: 'none' };

		/* 1. the autocomplete attribute is authoritative. */
		ac = parseAutocomplete(desc.autocomplete);
		if (ac.field !== undefined)
			return { field: ac.field, group: ac.group, via: 'autocomplete' };

		/* 2. heuristics, one source at a time. */
		for (i = 0; i < SOURCES.length; i++) {
			s = normalize(desc[SOURCES[i]]);
			if (s === '')
				continue;
			if (NEGATIVE.test(s))
				return { field: null, group: '', via: 'heuristic:' + SOURCES[i] };
			for (j = 0; j < HEURISTICS.length; j++) {
				if (HEURISTICS[j][1].test(s)) {
					field = HEURISTICS[j][0];
					/* A textarea holds the whole street address. */
					if (tag === 'textarea' && field === 'address_line1')
						field = 'street';
					return { field: field, group: '', via: 'heuristic:' + SOURCES[i] };
				}
			}
		}

		/* 3. the input type as the last resort. */
		if (tag === 'input' && (type === 'email' || type === 'tel'))
			return { field: type, group: '', via: 'type' };
		return { field: null, group: '', via: 'none' };
	}

	function isCardField(field) {
		return typeof field === 'string' && field.indexOf('cc_') === 0;
	}

	function pad2(n) {
		return (n < 10 ? '0' : '') + n;
	}

	/* Format an expiry for one combined field: "MM/YYYY" when the field
	 * asks for a four-digit year (placeholder or maxlength >= 7),
	 * otherwise "MM/YY". */
	function formatExpiry(month, year, placeholder, maxLength) {
		var p = String(placeholder || '').toLowerCase();
		var sep = /mm ?- ?yy/.test(p) ? '-' : '/';
		var four = /yyyy/.test(p) || (maxLength >= 7 && !/yy/.test(p));

		return pad2(month) + sep + (four ? String(year) : pad2(year % 100));
	}

	var core = {
		normalize: normalize,
		parseAutocomplete: parseAutocomplete,
		classify: classify,
		isCardField: isCardField,
		formatExpiry: formatExpiry
	};

	if (typeof globalThis !== 'undefined')
		globalThis.__gsurfAutofillCore = core;

	/* ================= 2. DOM side ================= */

	if (typeof window === 'undefined' || typeof document === 'undefined')
		return;
	/* Top-level documents only: frames never get suggestions or data. */
	if (window.top !== window)
		return;
	if (location.protocol !== 'http:' && location.protocol !== 'https:')
		return;
	if (window.__gsurfAutofill)
		return;

	var HANDLER = 'gsurfAutofill';

	var S = {
		armed: false,
		nonce: randomNonce(),
		host: location.hostname.toLowerCase(),
		labels: [],      /* profile labels, in order */
		card: [],        /* per label: has card data */
		suggest: true,
		target: null,    /* field the dropdown / pick belongs to */
		kind: null,      /* 'card' | 'address' */
		list: [],        /* labels shown in the open dropdown */
		sel: -1,
		host_el: null,   /* dropdown host element (closed shadow root) */
		items: [],
		dismissed: null, /* field whose dropdown the user dismissed */
		pending: null,   /* { target, kind } awaiting fill() */
		lastNotice: null,
		noticeEl: null
	};

	function randomNonce() {
		var a = new Uint8Array(16), s = '', i;

		crypto.getRandomValues(a);
		for (i = 0; i < a.length; i++)
			s += (a[i] < 16 ? '0' : '') + a[i].toString(16);
		return s;
	}

	function post(msg) {
		try {
			window.webkit.messageHandlers[HANDLER].postMessage(msg);
		} catch (e) {
			/* Not registered: the module is gone or disabled. */
		}
	}

	function secure() {
		return window.isSecureContext === true && location.protocol === 'https:';
	}

	/* ---- field discovery ---- */

	function labelText(el) {
		var parts = [], i, ids, ref;

		try {
			if (el.labels) {
				for (i = 0; i < el.labels.length; i++)
					parts.push(el.labels[i].textContent);
			}
		} catch (e) { /* ignore */ }
		if (parts.length === 0 && el.closest) {
			var wrap = el.closest('label');
			if (wrap)
				parts.push(wrap.textContent);
		}
		ids = el.getAttribute('aria-labelledby');
		if (ids) {
			ids.split(/\s+/).forEach(function (id) {
				ref = document.getElementById(id);
				if (ref)
					parts.push(ref.textContent);
			});
		}
		return parts.join(' ');
	}

	function describe(el) {
		return {
			tag: el.tagName,
			type: el.tagName === 'INPUT' ? (el.getAttribute('type') || 'text') : '',
			autocomplete: el.getAttribute('autocomplete'),
			name: el.getAttribute('name'),
			id: el.id,
			label: labelText(el),
			placeholder: el.getAttribute('placeholder'),
			aria: el.getAttribute('aria-label')
		};
	}

	function classifyEl(el) {
		if (!el || el.nodeType !== 1)
			return { field: null, group: '', via: 'none' };
		return classify(describe(el));
	}

	/* Fill only what the user can see: never disabled/read-only fields,
	 * nor fields hidden by display/visibility/opacity, collapsed to a
	 * point or positioned off the page (classic hidden-field autofill
	 * exfiltration). */
	function fillable(el) {
		var r, node, cs;

		if (el.disabled || el.readOnly)
			return false;
		if (el.getClientRects().length === 0)
			return false;
		r = el.getBoundingClientRect();
		if (r.width < 2 || r.height < 2)
			return false;
		if (r.right + window.scrollX <= 0 || r.bottom + window.scrollY <= 0)
			return false;
		for (node = el; node && node.nodeType === 1; node = node.parentElement) {
			cs = getComputedStyle(node);
			if (cs.display === 'none' || parseFloat(cs.opacity) === 0)
				return false;
			if (node === el && cs.visibility !== 'visible')
				return false;
		}
		return true;
	}

	/* The fields that share a form with @target (or, for a field outside
	 * any form, every form-less field of the document). */
	function scopeFields(target) {
		var all;

		if (target.form)
			return Array.prototype.slice.call(target.form.elements);
		all = document.querySelectorAll('input,select,textarea');
		return Array.prototype.filter.call(all, function (el) { return !el.form; });
	}

	/* ---- values ---- */

	function setValue(el, value) {
		var proto = el instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype
			: el instanceof HTMLSelectElement ? HTMLSelectElement.prototype
			: HTMLInputElement.prototype;
		var desc = Object.getOwnPropertyDescriptor(proto, 'value');

		/* The prototype's own setter (from this isolated world, so page
		 * scripts cannot have replaced it) bypasses instance-level value
		 * trackers that React-style frameworks install; the input and
		 * change events then tell the framework the value changed. */
		desc.set.call(el, value);
		el.dispatchEvent(new Event('input', { bubbles: true }));
		el.dispatchEvent(new Event('change', { bubbles: true }));
	}

	/* Index of the first option whose value or text equals one of
	 * @candidates (case-insensitive), then of one whose number equals
	 * @number. -1 when nothing matches. */
	function matchOption(el, candidates, number) {
		var opts = el.options, i, j, v, t, want;

		want = candidates.filter(function (c) { return c !== undefined && c !== null && c !== ''; })
			.map(function (c) { return String(c).toLowerCase().trim(); });
		for (i = 0; i < opts.length; i++) {
			v = String(opts[i].value).toLowerCase().trim();
			t = String(opts[i].text).toLowerCase().trim();
			for (j = 0; j < want.length; j++) {
				if (v === want[j] || t === want[j])
					return i;
			}
		}
		if (typeof number === 'number') {
			for (i = 0; i < opts.length; i++) {
				if (parseInt(opts[i].value, 10) === number ||
				    parseInt(opts[i].text, 10) === number)
					return i;
			}
		}
		return -1;
	}

	function countryNames(code) {
		var out = [], langs = ['en'], i;

		if (!code)
			return out;
		if (document.documentElement.lang)
			langs.unshift(document.documentElement.lang);
		if (navigator.language)
			langs.push(navigator.language);
		for (i = 0; i < langs.length; i++) {
			try {
				out.push(new Intl.DisplayNames([langs[i]], { type: 'region' }).of(code));
			} catch (e) { /* unknown language tag */ }
		}
		return out;
	}

	/* The value to write into @el for @field, or null to leave it. @ctx
	 * says which address line fields the form has. */
	function valueFor(field, v, el, ctx) {
		var lines = Array.isArray(v.street) ? v.street : [];
		var isSelect = el.tagName === 'SELECT';
		var idx, names, parts;

		switch (field) {
		case 'name':
			parts = [v.given_name, v.family_name].filter(function (p) { return !!p; });
			return parts.length > 0 ? parts.join(' ') : null;
		case 'street':
			if (lines.length === 0)
				return null;
			return lines.join(el.tagName === 'TEXTAREA' ? '\n' : ', ');
		case 'address_line1':
			if (lines.length === 0)
				return null;
			return ctx.line2 ? lines[0] : lines.join(', ');
		case 'address_line2':
			if (lines.length < 2)
				return null;
			return ctx.line3 ? lines[1] : lines.slice(1).join(', ');
		case 'address_line3':
			return lines.length >= 3 ? lines.slice(2).join(', ') : null;
		case 'cc_given_name':
			return v.cc_name ? v.cc_name.split(' ')[0] : null;
		case 'cc_family_name':
			return v.cc_name && v.cc_name.indexOf(' ') > 0
				? v.cc_name.slice(v.cc_name.indexOf(' ') + 1) : null;
		case 'cc_exp':
			if (!v.cc_exp_month || !v.cc_exp_year)
				return null;
			if (isSelect) {
				idx = matchOption(el, [
					formatExpiry(v.cc_exp_month, v.cc_exp_year, 'mm/yy', 0),
					formatExpiry(v.cc_exp_month, v.cc_exp_year, 'mm/yyyy', 0)]);
				return idx >= 0 ? el.options[idx].value : null;
			}
			return formatExpiry(v.cc_exp_month, v.cc_exp_year,
				el.getAttribute('placeholder'), el.maxLength);
		case 'cc_exp_month':
			if (!v.cc_exp_month)
				return null;
			if (isSelect) {
				idx = matchOption(el, [pad2(v.cc_exp_month), String(v.cc_exp_month)], v.cc_exp_month);
				return idx >= 0 ? el.options[idx].value : null;
			}
			return pad2(v.cc_exp_month);
		case 'cc_exp_year':
			if (!v.cc_exp_year)
				return null;
			if (isSelect) {
				idx = matchOption(el, [String(v.cc_exp_year), pad2(v.cc_exp_year % 100)]);
				return idx >= 0 ? el.options[idx].value : null;
			}
			return el.maxLength === 2 ? pad2(v.cc_exp_year % 100) : String(v.cc_exp_year);
		case 'country':
		case 'country_name':
			if (!v.country)
				return null;
			names = countryNames(v.country);
			if (isSelect) {
				idx = matchOption(el, [v.country].concat(names));
				return idx >= 0 ? el.options[idx].value : null;
			}
			return field === 'country_name' && names.length > 0 ? names[0] : v.country;
		case 'region':
			if (!v.region)
				return null;
			if (isSelect) {
				idx = matchOption(el, [v.region]);
				return idx >= 0 ? el.options[idx].value : null;
			}
			return v.region;
		case 'cc_csc':
			return null;   /* never stored, never filled */
		default:
			if (typeof v[field] !== 'string' || v[field] === '')
				return null;
			if (isSelect) {
				idx = matchOption(el, [v[field]]);
				return idx >= 0 ? el.options[idx].value : null;
			}
			return v[field];
		}
	}

	/* ---- shadow-DOM UI: dropdown and notices ---- */

	var CSS =
		':host{all:initial}' +
		'.box{font:13px/1.4 system-ui,sans-serif;background:#fff;color:#111;' +
		'border:1px solid #888;border-radius:4px;box-shadow:0 2px 8px rgba(0,0,0,.3);' +
		'min-width:10em;max-width:24em;overflow:hidden}' +
		'.head{font-size:11px;color:#666;padding:2px 8px;border-bottom:1px solid #ddd}' +
		'.item{padding:4px 8px;cursor:default;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}' +
		'.item.sel{background:#2a6fdb;color:#fff}' +
		'.note{padding:6px 10px}' +
		'@media (prefers-color-scheme: dark){.box{background:#222;color:#eee;border-color:#555}' +
		'.head{color:#aaa;border-color:#444}}';

	/* A fixed-position host element with a closed shadow root: the page
	 * sees an opaque element, never its contents. Inline !important
	 * styles keep page CSS from hiding or moving it. */
	function makeHost(tag) {
		var host = document.createElement('div');
		var root = host.attachShadow({ mode: 'closed' });
		var style = document.createElement('style');
		var box = document.createElement('div');

		host.setAttribute('data-gsurf-autofill', tag);
		host.style.setProperty('all', 'initial', 'important');
		host.style.setProperty('position', 'fixed', 'important');
		host.style.setProperty('z-index', '2147483647', 'important');
		host.style.setProperty('display', 'block', 'important');
		style.textContent = CSS;
		box.className = 'box';
		root.appendChild(style);
		root.appendChild(box);
		return { host: host, box: box };
	}

	function place(host, target) {
		var r = target.getBoundingClientRect();

		host.style.setProperty('left', Math.max(0, r.left) + 'px', 'important');
		host.style.setProperty('top', (r.bottom + 2) + 'px', 'important');
	}

	function hideDropdown() {
		if (S.host_el && S.host_el.parentNode)
			S.host_el.parentNode.removeChild(S.host_el);
		S.host_el = null;
		S.items = [];
		S.list = [];
		S.sel = -1;
	}

	function labelsFor(kind) {
		return S.labels.filter(function (l, i) { return kind !== 'card' || S.card[i]; });
	}

	function highlight(i) {
		S.sel = i;
		S.items.forEach(function (it, j) {
			it.className = j === i ? 'item sel' : 'item';
		});
	}

	function showDropdown(target, kind, preselect) {
		var ui, head;

		hideDropdown();
		S.list = labelsFor(kind);
		if (S.list.length === 0)
			return false;
		S.target = target;
		S.kind = kind;
		ui = makeHost('suggest');
		head = document.createElement('div');
		head.className = 'head';
		head.textContent = kind === 'card' ? 'Autofill card' : 'Autofill';
		ui.box.appendChild(head);
		S.items = S.list.map(function (label, i) {
			var it = document.createElement('div');

			it.className = 'item';
			it.setAttribute('role', 'option');
			it.textContent = label;
			it.addEventListener('mousemove', function () { highlight(i); });
			it.addEventListener('click', function (e) {
				if (e.isTrusted)
					choose(i);
			});
			ui.box.appendChild(it);
			return it;
		});
		/* Keep focus in the field while clicking the list. */
		ui.host.addEventListener('mousedown', function (e) { e.preventDefault(); }, true);
		place(ui.host, target);
		document.documentElement.appendChild(ui.host);
		S.host_el = ui.host;
		highlight(preselect ? 0 : -1);
		return true;
	}

	var NOTICES = {
		insecure: 'Card details are only filled on https pages.',
		nocard: 'This autofill profile has no card details.',
		nofield: 'Focus a form field to autofill it.',
		noprofiles: 'No autofill profiles are configured.',
		nofill: 'Nothing in this form could be autofilled.',
		unknown: 'That autofill profile no longer exists.'
	};

	function notice(code) {
		var ui = makeHost('notice'), note = document.createElement('div');

		S.lastNotice = code;
		if (S.noticeEl && S.noticeEl.parentNode)
			S.noticeEl.parentNode.removeChild(S.noticeEl);
		note.className = 'note';
		note.textContent = NOTICES[code] || code;
		ui.box.appendChild(note);
		ui.host.style.setProperty('right', '16px', 'important');
		ui.host.style.setProperty('bottom', '16px', 'important');
		document.documentElement.appendChild(ui.host);
		S.noticeEl = ui.host;
		setTimeout(function () {
			if (ui.host.parentNode)
				ui.host.parentNode.removeChild(ui.host);
		}, 4000);
	}

	/* ---- picking and filling ---- */

	/* The user chose entry @i of the open list: ask the host for that
	 * profile's values (by label, so a reordered config cannot swap
	 * profiles under the user). */
	function choose(i) {
		var label = S.list[i];

		if (label === undefined || !S.target)
			return null;
		S.pending = { target: S.target, kind: S.kind };
		S.dismissed = S.target;
		hideDropdown();
		post({ type: 'pick', nonce: S.nonce, label: label, kind: S.pending.kind });
		return label;
	}

	/* Arrow keys move, Enter picks, Escape closes. Returns true when the
	 * key was ours (the caller then stops it reaching the page). */
	function key(name) {
		var n = S.items.length;

		if (!S.host_el || n === 0)
			return false;
		switch (name) {
		case 'ArrowDown':
			highlight(S.sel < 0 ? 0 : (S.sel + 1) % n);
			return true;
		case 'ArrowUp':
			highlight(S.sel <= 0 ? n - 1 : S.sel - 1);
			return true;
		case 'Enter':
			if (S.sel < 0)
				return false;
			choose(S.sel);
			return true;
		case 'Escape':
			S.dismissed = S.target;
			hideDropdown();
			return true;
		default:
			return false;
		}
	}

	function onFocusIn(e) {
		var el = e.target, info;

		if (!S.armed || !S.suggest)
			return;
		info = classifyEl(el);
		if (!info.field || info.field === 'cc_csc' || el === S.dismissed) {
			if (el !== S.target)
				hideDropdown();
			return;
		}
		showDropdown(el, isCardField(info.field) ? 'card' : 'address', false);
	}

	function onFocusOut() {
		setTimeout(function () {
			if (document.activeElement !== S.target)
				hideDropdown();
			if (document.activeElement !== S.dismissed)
				S.dismissed = null;
		}, 150);
	}

	function onKeyDown(e) {
		if (!e.isTrusted || !S.host_el)
			return;
		if (key(e.key)) {
			e.preventDefault();
			e.stopImmediatePropagation();
		}
	}

	function onViewport() {
		if (S.host_el && S.target)
			place(S.host_el, S.target);
	}

	/* Host -> page: enable the module for this document. The host checks
	 * the enabled flag and the exclude rules before calling this, and
	 * names the host it checked so a reply that arrives after a
	 * navigation is ignored. */
	function arm(cfg) {
		if (!cfg || cfg.nonce !== S.nonce || cfg.host !== S.host)
			return 'stale';
		S.labels = Array.isArray(cfg.labels) ? cfg.labels.map(String) : [];
		S.card = Array.isArray(cfg.card) ? cfg.card.map(Boolean) : [];
		S.suggest = cfg.suggest !== false;
		if (!S.armed) {
			document.addEventListener('focusin', onFocusIn, true);
			document.addEventListener('focusout', onFocusOut, true);
			window.addEventListener('keydown', onKeyDown, true);
			window.addEventListener('scroll', onViewport, { passive: true, capture: true });
			window.addEventListener('resize', onViewport, { passive: true });
		}
		S.armed = true;
		/* A field focused before arming (autofocus) gets its list now. */
		if (S.suggest && document.activeElement && document.activeElement !== document.body)
			onFocusIn({ target: document.activeElement });
		return 'armed';
	}

	/* Host -> page: the fill key or the form-autofill action. One
	 * candidate profile is picked at once; several open the list. */
	function trigger(cfg) {
		var el = document.activeElement, info, kind, list;

		if (!S.armed || !cfg || cfg.host !== S.host)
			return 'unarmed';
		info = classifyEl(el);
		if (!el || !info.field || info.field === 'cc_csc') {
			notice('nofield');
			return 'nofield';
		}
		kind = isCardField(info.field) ? 'card' : 'address';
		list = labelsFor(kind);
		if (list.length === 0) {
			notice(kind === 'card' ? 'nocard' : 'noprofiles');
			return 'noprofile';
		}
		if (!showDropdown(el, kind, true))
			return 'noprofile';
		if (list.length === 1) {
			choose(0);
			return 'pick';
		}
		return 'picker';
	}

	/* Host -> page: the chosen profile's values for the pending field's
	 * form (only the card part or only the address part), or a refusal.
	 * Returns a summary without values. */
	function fill(cfg) {
		var p = S.pending, fields, ctx, filled = [], target, group;

		if (!cfg || cfg.nonce !== S.nonce || cfg.host !== S.host)
			return { error: 'stale' };
		S.pending = null;
		if (cfg.refused) {
			notice(cfg.refused);
			return { refused: cfg.refused };
		}
		if (!p || !p.target || !p.target.isConnected || p.kind !== cfg.kind)
			return { error: 'notarget' };
		/* Defence in depth: the host already refused card data here. */
		if (p.kind === 'card' && !secure()) {
			notice('insecure');
			return { refused: 'insecure' };
		}
		target = p.target;
		group = classifyEl(target).group;
		fields = scopeFields(target).map(function (el) {
			return { el: el, info: classifyEl(el) };
		}).filter(function (f) {
			if (!f.info.field || f.info.field === 'cc_csc')
				return false;
			if (isCardField(f.info.field) !== (p.kind === 'card'))
				return false;
			/* A tagged section fills itself and untagged fields only. */
			if (group !== '' && f.info.group !== '' && f.info.group !== group)
				return false;
			return fillable(f.el);
		});
		ctx = {
			line2: fields.some(function (f) { return f.info.field === 'address_line2'; }),
			line3: fields.some(function (f) { return f.info.field === 'address_line3'; })
		};
		fields.forEach(function (f) {
			var value = valueFor(f.info.field, cfg.values || {}, f.el, ctx);

			if (value === null || value === undefined)
				return;
			setValue(f.el, value);
			filled.push(f.info.field);
		});
		if (filled.length === 0)
			notice('nofill');
		return { filled: filled.length, fields: filled };
	}

	/* What the tests (and only the host) may inspect: state without
	 * values, since this world never holds any outside fill(). */
	function state() {
		return {
			armed: S.armed,
			host: S.host,
			labels: S.labels.slice(),
			card: S.card.slice(),
			suggest: S.suggest,
			open: S.host_el !== null,
			list: S.list.slice(),
			sel: S.sel,
			kind: S.kind,
			pending: S.pending !== null,
			lastNotice: S.lastNotice
		};
	}

	window.__gsurfAutofill = {
		arm: arm,
		trigger: trigger,
		fill: fill,
		choose: choose,
		key: key,
		state: state,
		classifyElement: classifyEl,
		core: core
	};

	post({ type: 'ready', nonce: S.nonce });
})();
