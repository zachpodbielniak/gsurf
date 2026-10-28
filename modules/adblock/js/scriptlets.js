// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Zach Podbielniak
//
// gsurf adblock scriptlet library.
//
// Scriptlets neutralize anti-adblock and ad code from inside the page:
// they run in the page's main world, in every frame, before any of the
// page's own scripts. These are independent implementations of the
// behaviour uBlock Origin's scriptlet names describe, so its filter lists
// (`example.com##+js(set, foo.bar, false)`) work unchanged.
//
// File format, read by gsurf-adblock-filters.c:
//   /// helpers                      code shared by every injection
//   /// scriptlet NAME ALIAS...      one scriptlet; the section's first
//                                    `function NAME(` is what gets called
//   /// scriptlet !trusted NAME ...  only lists marked trusted may use it
// The web-process extension wraps the helpers, the scriptlets a frame
// needs and their calls in a single closure, so nothing here leaks into
// the page's global scope.

/// helpers
// "/re/flags" or a literal string -> RegExp. An empty pattern matches all.
function gsurfPattern(pattern, flags) {
	if (pattern === undefined || pattern === '') {
		return /^/;
	}
	const m = /^\/(.+)\/([dgimsuy]*)$/.exec(pattern);
	if (m !== null) {
		try {
			return new RegExp(m[1], flags !== undefined ? flags : m[2]);
		} catch (e) {
			return /[^\s\S]/;
		}
	}
	return new RegExp(pattern.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), flags);
}

// A leading "!" inverts a needle: returns [pattern-without-!, inverted].
function gsurfNeedle(needle) {
	needle = needle === undefined ? '' : String(needle);
	if (needle.startsWith('!')) {
		return [ needle.slice(1), true ];
	}
	return [ needle, false ];
}

// Map uBO's constant names onto values. Returns gsurfConstant itself for
// anything not on the safe list, which callers treat as "do nothing".
function gsurfConstant(raw) {
	const table = {
		'undefined': undefined, 'false': false, 'true': true, 'null': null,
		"''": '', 'emptyStr': '', 'emptyObj': {}, 'emptyArr': [],
		'noopFunc': function () {}, 'trueFunc': function () { return true; },
		'falseFunc': function () { return false; },
		'throwFunc': function () { throw new Error(); },
		'noopCallbackFunc': function () { return function () {}; },
		'noopThisFunc': function () { return this; },
		'NaN': NaN, 'Infinity': Infinity, '-Infinity': -Infinity,
		'yes': 'yes', 'no': 'no', 'on': 'on', 'off': 'off',
		'accept': 'accept', 'accepted': 'accepted', 'reject': 'reject',
		'rejected': 'rejected', 'allow': 'allow', 'deny': 'deny', 'ok': 'ok',
	};
	if (Object.prototype.hasOwnProperty.call(table, raw)) {
		return table[raw];
	}
	if (/^-?\d+(\.\d+)?$/.test(raw) && Math.abs(Number(raw)) <= 0x7fff) {
		return Number(raw);
	}
	if (/^'.*'$/.test(raw)) {
		return raw.slice(1, -1);
	}
	return gsurfConstant;
}

function gsurfIsObject(v) {
	return v !== null && (typeof v === 'object' || typeof v === 'function');
}

// Walk a dotted property chain from window and install a property
// descriptor on its last link. Missing links are trapped so the leaf is
// installed as soon as the page creates the object holding it.
// leaf(owner, prop, existingDescriptor) returns the descriptor or null.
function gsurfTrap(chain, leaf) {
	const trap = function (owner, path) {
		const dot = path.indexOf('.');
		const prop = dot === -1 ? path : path.slice(0, dot);
		let d;
		try {
			d = Object.getOwnPropertyDescriptor(owner, prop);
		} catch (e) {
			return;
		}
		if (dot === -1) {
			const desc = leaf(owner, prop, d);
			if (desc === null || (d !== undefined && d.configurable === false)) {
				return;
			}
			try {
				Object.defineProperty(owner, prop, desc);
			} catch (e) {
			}
			return;
		}
		const rest = path.slice(dot + 1);
		let current = owner[prop];
		if (gsurfIsObject(current)) {
			trap(current, rest);
			return;
		}
		if (d !== undefined && d.configurable === false) {
			return;
		}
		try {
			Object.defineProperty(owner, prop, {
				configurable: true,
				enumerable: true,
				get: function () { return current; },
				set: function (v) {
					current = v;
					if (gsurfIsObject(v)) {
						trap(v, rest);
					}
				},
			});
		} catch (e) {
		}
	};
	if (typeof chain === 'string' && chain !== '') {
		trap(window, chain);
	}
}

// Run fn once the DOM is ready and, with "stay", again on every change.
function gsurfOnDom(fn, behavior) {
	let pending = false;
	const run = function () {
		pending = false;
		try {
			fn();
		} catch (e) {
		}
	};
	const later = function () {
		if (!pending) {
			pending = true;
			setTimeout(run, 0);
		}
	};
	if (document.readyState === 'loading') {
		document.addEventListener('DOMContentLoaded', run, { once: true });
	} else {
		run();
	}
	if (behavior === 'stay') {
		new MutationObserver(later).observe(document, { childList: true, subtree: true, attributes: true });
	} else {
		window.addEventListener('load', run, { once: true });
	}
}

// Parse "key:value key:value" request matchers (bare value = url).
function gsurfRequestNeedles(props) {
	const keys = [ 'url', 'method', 'headers', 'body', 'mode', 'credentials', 'cache',
		'redirect', 'referrer', 'referrerPolicy', 'integrity', 'keepalive', 'priority' ];
	const needles = [];
	String(props).split(/\s+/).forEach(function (part) {
		if (part === '' || part === '*') {
			return;
		}
		const i = part.indexOf(':');
		let key = 'url';
		let value = part;
		if (i > 0 && keys.includes(part.slice(0, i))) {
			key = part.slice(0, i);
			value = part.slice(i + 1);
		}
		needles.push([ key, gsurfPattern(value) ]);
	});
	return needles;
}

function gsurfRequestMatches(needles, details) {
	return needles.every(function (n) {
		const v = details[n[0]];
		return v !== undefined && n[1].test(String(v));
	});
}

function gsurfFakeBody(kind) {
	if (kind === 'emptyObj') {
		return '{}';
	}
	if (kind === 'emptyArr') {
		return '[]';
	}
	return '';
}

// Delete (or just test for) a dotted path; "*" and "[]" match any key.
function gsurfPath(root, path, remove) {
	const dot = path.indexOf('.');
	const key = dot === -1 ? path : path.slice(0, dot);
	const rest = dot === -1 ? '' : path.slice(dot + 1);
	if (!gsurfIsObject(root)) {
		return false;
	}
	const keys = (key === '*' || key === '[]') ? Object.keys(root) : [ key ];
	let found = false;
	for (const k of keys) {
		if (!Object.prototype.hasOwnProperty.call(root, k)) {
			continue;
		}
		if (rest === '') {
			found = true;
			if (remove) {
				delete root[k];
			}
		} else if (gsurfPath(root[k], rest, remove)) {
			found = true;
		}
	}
	return found;
}

function gsurfPrune(obj, prunePaths, needlePaths) {
	if (!gsurfIsObject(obj) || prunePaths === undefined || prunePaths === '') {
		return obj;
	}
	const needles = needlePaths ? needlePaths.split(/ +/).filter(Boolean) : [];
	if (!needles.every(function (p) { return gsurfPath(obj, p, false); })) {
		return obj;
	}
	prunePaths.split(/ +/).filter(Boolean).forEach(function (p) {
		gsurfPath(obj, p, true);
	});
	return obj;
}

function gsurfDefuseTimer(name, needle, delay) {
	const [ pattern, notPattern ] = gsurfNeedle(needle);
	const [ wait, notWait ] = gsurfNeedle(delay);
	if (pattern === '' && wait === '') {
		return;
	}
	const re = gsurfPattern(pattern);
	const ms = parseInt(wait, 10);
	window[name] = new Proxy(window[name], {
		apply: function (target, thisArg, args) {
			let defuse = true;
			if (pattern !== '') {
				defuse = re.test(String(args[0])) !== notPattern;
			}
			if (defuse && !isNaN(ms)) {
				defuse = (args[1] === ms) !== notWait;
			}
			if (defuse) {
				args[0] = function () {};
			}
			return Reflect.apply(target, thisArg, args);
		},
	});
}

function gsurfAdjustTimer(name, needle, delay, boost) {
	const re = gsurfPattern(needle === undefined || needle === '*' ? '' : needle);
	const ms = delay === '*' ? NaN : parseInt(delay === undefined || delay === '' ? '1000' : delay, 10);
	let factor = parseFloat(boost === undefined || boost === '' ? '0.05' : boost);
	if (isNaN(factor) || !isFinite(factor)) {
		factor = 0.05;
	}
	factor = Math.min(Math.max(factor, 0.001), 50);
	window[name] = new Proxy(window[name], {
		apply: function (target, thisArg, args) {
			if ((isNaN(ms) || args[1] === ms) && re.test(String(args[0]))) {
				args[1] = args[1] * factor;
			}
			return Reflect.apply(target, thisArg, args);
		},
	});
}

function gsurfAbortError() {
	return new ReferenceError('gsurf: blocked');
}

// Blank (or rewrite) the text of matching nodes as the parser inserts
// them: nodeName "script" for inline scripts, "#text" for text nodes.
function gsurfReplaceNodeText(nodeName, pattern, replacement, replace) {
	if (!nodeName || pattern === undefined || pattern === '') {
		return;
	}
	const name = nodeName.toUpperCase();
	const re = gsurfPattern(pattern);
	const global = replace ? gsurfPattern(pattern, re.flags.includes('g') ? re.flags : re.flags + 'g') : null;
	const handle = function (node) {
		if (node.nodeName.toUpperCase() !== name) {
			return;
		}
		const text = node.textContent;
		if (!re.test(text)) {
			return;
		}
		node.textContent = replace ? text.replace(global, replacement) : '';
	};
	new MutationObserver(function (records) {
		for (const record of records) {
			for (const node of record.addedNodes) {
				handle(node);
			}
		}
	}).observe(document, { childList: true, subtree: true });
}

// Write one storage item. Untrusted lists may only store harmless values.
function gsurfSetStorage(storage, key, value, trusted) {
	if (!key || value === undefined) {
		return;
	}
	try {
		if (value === '$remove$') {
			storage.removeItem(key);
			return;
		}
		if (!trusted) {
			const table = { 'emptyArr': '[]', 'emptyObj': '{}', "''": '' };
			if (Object.prototype.hasOwnProperty.call(table, value)) {
				value = table[value];
			} else if (!/^(undefined|false|true|null|yes|no|on|off|-?\d{1,5}|)$/.test(value)) {
				return;
			}
		}
		storage.setItem(key, String(value));
	} catch (e) {
	}
}

function gsurfWriteCookie(name, value, path) {
	const pair = encodeURIComponent(name) + '=' + encodeURIComponent(value);
	if (document.cookie.split(';').some(function (c) { return c.trim() === pair; })) {
		return;
	}
	document.cookie = pair + (path === 'none' ? '' : '; path=/');
}

/// scriptlet set-constant set
function setConstant(chain, raw) {
	const value = gsurfConstant(raw);
	if (value === gsurfConstant) {
		return;
	}
	gsurfTrap(chain, function (owner, prop, d) {
		if (d !== undefined && d.configurable === false) {
			try {
				owner[prop] = value;
			} catch (e) {
			}
			return null;
		}
		return {
			configurable: true,
			enumerable: true,
			get: function () { return value; },
			set: function () {},
		};
	});
}

/// scriptlet !trusted trusted-set-constant trusted-set
function trustedSetConstant(chain, raw) {
	let value;
	try {
		value = JSON.parse(raw);
	} catch (e) {
		value = gsurfConstant(raw);
		if (value === gsurfConstant) {
			value = raw;
		}
	}
	gsurfTrap(chain, function () {
		return {
			configurable: true,
			enumerable: true,
			get: function () { return value; },
			set: function () {},
		};
	});
}

/// scriptlet abort-on-property-read aopr
function abortOnPropertyRead(chain) {
	const err = gsurfAbortError();
	gsurfTrap(chain, function () {
		return {
			configurable: true,
			get: function () { throw err; },
			set: function () {},
		};
	});
}

/// scriptlet abort-on-property-write aopw
function abortOnPropertyWrite(chain) {
	const err = gsurfAbortError();
	gsurfTrap(chain, function (owner, prop) {
		const value = owner[prop];
		return {
			configurable: true,
			get: function () { return value; },
			set: function () { throw err; },
		};
	});
}

/// scriptlet abort-current-script acs abort-current-inline-script acis
function abortCurrentScript(chain, needle) {
	const err = gsurfAbortError();
	const [ pattern, inverted ] = gsurfNeedle(needle);
	const re = gsurfPattern(pattern);
	const self = document.currentScript;
	const check = function () {
		const e = document.currentScript;
		if (!(e instanceof HTMLScriptElement) || e === self) {
			return;
		}
		const text = e.src ? e.src : e.textContent;
		if (pattern !== '' && re.test(text) === inverted) {
			return;
		}
		throw err;
	};
	gsurfTrap(chain, function (owner, prop, d) {
		let value = owner[prop];
		const get = d && d.get;
		const set = d && d.set;
		return {
			configurable: true,
			enumerable: d === undefined || d.enumerable,
			get: function () {
				check();
				return get ? get.call(this) : value;
			},
			set: function (v) {
				check();
				if (set) {
					set.call(this, v);
				} else {
					value = v;
				}
			},
		};
	});
}

/// scriptlet abort-on-stack-trace aost
function abortOnStackTrace(chain, needle) {
	const err = gsurfAbortError();
	const re = gsurfPattern(needle);
	const check = function () {
		if (re.test(new Error().stack || '')) {
			throw err;
		}
	};
	gsurfTrap(chain, function (owner, prop, d) {
		let value = owner[prop];
		const get = d && d.get;
		const set = d && d.set;
		return {
			configurable: true,
			get: function () {
				check();
				return get ? get.call(this) : value;
			},
			set: function (v) {
				check();
				if (set) {
					set.call(this, v);
				} else {
					value = v;
				}
			},
		};
	});
}

/// scriptlet no-setTimeout-if nostif prevent-setTimeout setTimeout-defuser
function noSetTimeoutIf(needle, delay) {
	gsurfDefuseTimer('setTimeout', needle, delay);
}

/// scriptlet no-setInterval-if nosiif prevent-setInterval setInterval-defuser
function noSetIntervalIf(needle, delay) {
	gsurfDefuseTimer('setInterval', needle, delay);
}

/// scriptlet adjust-setTimeout nano-setTimeout-booster nano-stb
function adjustSetTimeout(needle, delay, boost) {
	gsurfAdjustTimer('setTimeout', needle, delay, boost);
}

/// scriptlet adjust-setInterval nano-setInterval-booster nano-sib
function adjustSetInterval(needle, delay, boost) {
	gsurfAdjustTimer('setInterval', needle, delay, boost);
}

/// scriptlet no-requestAnimationFrame-if norafif prevent-requestAnimationFrame
function noRequestAnimationFrameIf(needle) {
	const [ pattern, inverted ] = gsurfNeedle(needle);
	const re = gsurfPattern(pattern);
	window.requestAnimationFrame = new Proxy(window.requestAnimationFrame, {
		apply: function (target, thisArg, args) {
			if (re.test(String(args[0])) !== inverted) {
				args[0] = function () {};
			}
			return Reflect.apply(target, thisArg, args);
		},
	});
}

/// scriptlet addEventListener-defuser aeld prevent-addEventListener
function addEventListenerDefuser(type, needle) {
	const [ typePattern, typeInverted ] = gsurfNeedle(type);
	const [ handlerPattern, handlerInverted ] = gsurfNeedle(needle);
	const typeRe = gsurfPattern(typePattern);
	const handlerRe = gsurfPattern(handlerPattern);
	const proto = EventTarget.prototype;
	proto.addEventListener = new Proxy(proto.addEventListener, {
		apply: function (target, thisArg, args) {
			let handler = '';
			try {
				const h = args[1];
				handler = String(h instanceof Function ? h : h && h.handleEvent);
			} catch (e) {
			}
			if (typeRe.test(String(args[0])) !== typeInverted &&
			    handlerRe.test(handler) !== handlerInverted) {
				return undefined;
			}
			return Reflect.apply(target, thisArg, args);
		},
	});
}

/// scriptlet no-fetch-if prevent-fetch
function noFetchIf(props, body) {
	if (props === undefined || props === '') {
		return;
	}
	const needles = gsurfRequestNeedles(props);
	const text = gsurfFakeBody(body);
	window.fetch = new Proxy(window.fetch, {
		apply: function (target, thisArg, args) {
			const details = {};
			const r = args[0];
			if (r instanceof Request) {
				details.url = r.url;
				details.method = r.method;
				details.mode = r.mode;
				details.credentials = r.credentials;
			} else {
				details.url = String(r);
			}
			const init = args[1];
			if (gsurfIsObject(init)) {
				for (const k in init) {
					try {
						details[k] = String(init[k]);
					} catch (e) {
					}
				}
			}
			if (!gsurfRequestMatches(needles, details)) {
				return Reflect.apply(target, thisArg, args);
			}
			return Promise.resolve(new Response(text, { status: 200, statusText: 'OK' }));
		},
	});
}

/// scriptlet no-xhr-if prevent-xhr
function noXhrIf(props, body) {
	if (props === undefined || props === '') {
		return;
	}
	const needles = gsurfRequestNeedles(props);
	const text = gsurfFakeBody(body);
	const blocked = new WeakMap();
	const proto = XMLHttpRequest.prototype;
	const open = proto.open;
	const send = proto.send;
	proto.open = function (method, url) {
		const details = { method: String(method), url: String(url) };
		if (gsurfRequestMatches(needles, details)) {
			blocked.set(this, details);
		} else {
			blocked.delete(this);
		}
		return open.apply(this, arguments);
	};
	proto.send = function () {
		const details = blocked.get(this);
		if (details === undefined) {
			return send.apply(this, arguments);
		}
		const xhr = this;
		const fixed = { readyState: 4, status: 200, statusText: 'OK',
			responseText: text, response: text, responseURL: details.url };
		for (const k in fixed) {
			Object.defineProperty(xhr, k, { value: fixed[k], configurable: true });
		}
		setTimeout(function () {
			[ 'readystatechange', 'load', 'loadend' ].forEach(function (t) {
				xhr.dispatchEvent(new Event(t));
			});
		}, 1);
	};
}

/// scriptlet json-prune
function jsonPrune(prunePaths, needlePaths) {
	if (prunePaths === undefined || prunePaths === '') {
		return;
	}
	JSON.parse = new Proxy(JSON.parse, {
		apply: function (target, thisArg, args) {
			return gsurfPrune(Reflect.apply(target, thisArg, args), prunePaths, needlePaths);
		},
	});
	Response.prototype.json = new Proxy(Response.prototype.json, {
		apply: function (target, thisArg, args) {
			return Reflect.apply(target, thisArg, args).then(function (obj) {
				return gsurfPrune(obj, prunePaths, needlePaths);
			});
		},
	});
}

/// scriptlet json-prune-fetch-response
function jsonPruneFetchResponse(prunePaths, needlePaths) {
	if (prunePaths === undefined || prunePaths === '') {
		return;
	}
	let needles = [];
	for (let i = 2; i + 1 < arguments.length; i += 2) {
		if (arguments[i] === 'propsToMatch') {
			needles = gsurfRequestNeedles(arguments[i + 1]);
		}
	}
	window.fetch = new Proxy(window.fetch, {
		apply: function (target, thisArg, args) {
			const r = args[0];
			const url = r instanceof Request ? r.url : String(r);
			const promise = Reflect.apply(target, thisArg, args);
			if (!gsurfRequestMatches(needles, { url: url })) {
				return promise;
			}
			return promise.then(function (response) {
				return response.clone().json().then(function (obj) {
					const pruned = gsurfPrune(obj, prunePaths, needlePaths);
					return new Response(JSON.stringify(pruned), {
						status: response.status,
						statusText: response.statusText,
						headers: response.headers,
					});
				}).catch(function () {
					return response;
				});
			});
		},
	});
}

/// scriptlet window.open-defuser nowoif no-window-open-if prevent-window-open
function noWindowOpenIf(pattern) {
	const [ p, inverted ] = gsurfNeedle(pattern);
	const re = gsurfPattern(p);
	const decoy = function () {
		return new Proxy({}, {
			get: function (target, prop) {
				if (prop === 'closed') {
					return false;
				}
				if (prop === 'document' || prop === 'location' || prop === 'window' || prop === 'self') {
					return this;
				}
				return function () {};
			},
			set: function () { return true; },
		});
	};
	window.open = new Proxy(window.open, {
		apply: function (target, thisArg, args) {
			const url = args.length === 0 ? '' : String(args[0]);
			if (re.test(url) === inverted) {
				return Reflect.apply(target, thisArg, args);
			}
			return decoy();
		},
	});
}

/// scriptlet noeval silent-noeval noeval-silent
function noEval() {
	window.eval = new Proxy(window.eval, {
		apply: function () {
			return undefined;
		},
	});
}

/// scriptlet noeval-if prevent-eval-if
function noEvalIf(needle) {
	const [ p, inverted ] = gsurfNeedle(needle);
	const re = gsurfPattern(p);
	window.eval = new Proxy(window.eval, {
		apply: function (target, thisArg, args) {
			if (re.test(String(args[0])) !== inverted) {
				return undefined;
			}
			return Reflect.apply(target, thisArg, args);
		},
	});
}

/// scriptlet nowebrtc
function noWebrtc() {
	const Stub = function () {
		this.close = function () {};
		this.createDataChannel = function () {};
		this.createOffer = function () { return Promise.reject(new Error('blocked')); };
		this.setRemoteDescription = function () { return Promise.reject(new Error('blocked')); };
		this.addEventListener = function () {};
	};
	[ 'RTCPeerConnection', 'webkitRTCPeerConnection' ].forEach(function (name) {
		if (name in window) {
			window[name] = Stub;
		}
	});
}

/// scriptlet remove-attr ra
function removeAttr(attrs, selector, behavior) {
	if (attrs === undefined || attrs === '') {
		return;
	}
	const names = attrs.split(/\s*\|\s*/).filter(Boolean);
	const sel = selector || names.map(function (n) { return '[' + n + ']'; }).join(',');
	gsurfOnDom(function () {
		document.querySelectorAll(sel).forEach(function (el) {
			names.forEach(function (n) {
				el.removeAttribute(n);
			});
		});
	}, behavior);
}

/// scriptlet remove-class rc
function removeClass(classes, selector, behavior) {
	if (classes === undefined || classes === '') {
		return;
	}
	const names = classes.split(/\s*\|\s*/).filter(Boolean);
	const sel = selector || names.map(function (n) { return '.' + CSS.escape(n); }).join(',');
	gsurfOnDom(function () {
		document.querySelectorAll(sel).forEach(function (el) {
			el.classList.remove.apply(el.classList, names);
		});
	}, behavior);
}

/// scriptlet set-attr
function setAttr(selector, attr, value) {
	if (!selector || !attr) {
		return;
	}
	value = value === undefined ? '' : value;
	const copy = /^\[.+\]$/.test(value) ? value.slice(1, -1) : null;
	if (copy === null && !/^(|true|false|-?\d+)$/.test(value)) {
		return;
	}
	gsurfOnDom(function () {
		document.querySelectorAll(selector).forEach(function (el) {
			const v = copy === null ? value : el.getAttribute(copy);
			if (v !== null) {
				el.setAttribute(attr, v);
			}
		});
	}, 'stay');
}

/// scriptlet remove-node-text rmnt
function removeNodeText(nodeName, needle) {
	gsurfReplaceNodeText(nodeName, needle, '');
}

/// scriptlet !trusted trusted-replace-node-text rpnt replace-node-text
function replaceNodeText(nodeName, pattern, replacement) {
	gsurfReplaceNodeText(nodeName, pattern, replacement === undefined ? '' : replacement, true);
}

/// scriptlet set-local-storage-item
function setLocalStorageItem(key, value) {
	gsurfSetStorage(window.localStorage, key, value, false);
}

/// scriptlet set-session-storage-item
function setSessionStorageItem(key, value) {
	gsurfSetStorage(window.sessionStorage, key, value, false);
}

/// scriptlet !trusted trusted-set-local-storage-item
function trustedSetLocalStorageItem(key, value) {
	gsurfSetStorage(window.localStorage, key, value, true);
}

/// scriptlet set-cookie
function setCookie(name, value, path) {
	const safe = /^(true|false|yes|no|y|n|on|off|ok|accept|accepted|reject|rejected|allow|allowed|deny|denied|necessary|required|hide|hidden|essential|nonessential|checked|unchecked|forbidden|forever|-?\d{1,5}|)$/i;
	if (!name || value === undefined || !safe.test(value)) {
		return;
	}
	gsurfWriteCookie(name, value, path);
}

/// scriptlet !trusted trusted-set-cookie
function trustedSetCookie(name, value, offset, path) {
	if (!name || value === undefined) {
		return;
	}
	gsurfWriteCookie(name, value.replace('$now$', String(Date.now())), path);
}

/// scriptlet remove-cookie cookie-remover
function removeCookie(needle) {
	const re = gsurfPattern(needle);
	const remove = function () {
		document.cookie.split(';').forEach(function (c) {
			const name = c.split('=')[0].trim();
			if (name === '' || !re.test(name)) {
				return;
			}
			const expire = name + '=; Max-Age=-1000';
			const host = location.hostname;
			document.cookie = expire;
			document.cookie = expire + '; path=/';
			document.cookie = expire + '; domain=' + host + '; path=/';
			document.cookie = expire + '; domain=.' + host + '; path=/';
		});
	};
	remove();
	window.addEventListener('beforeunload', remove);
	document.addEventListener('visibilitychange', remove);
}

/// scriptlet refresh-defuser prevent-refresh
function refreshDefuser(delay) {
	const wanted = delay === undefined || delay === '' ? NaN : parseInt(delay, 10);
	gsurfOnDom(function () {
		const meta = document.querySelector('meta[http-equiv="refresh" i][content]');
		if (meta === null) {
			return;
		}
		const secs = parseInt(meta.getAttribute('content'), 10);
		if (isNaN(wanted) || wanted === secs) {
			window.stop();
		}
	});
}

/// scriptlet href-sanitizer
function hrefSanitizer(selector, source) {
	if (!selector) {
		return;
	}
	source = source || 'text';
	const extract = function (el) {
		if (source === 'text') {
			return el.textContent.trim();
		}
		if (/^\[.+\]$/.test(source)) {
			return el.getAttribute(source.slice(1, -1));
		}
		if (source.startsWith('?')) {
			try {
				return new URL(el.href).searchParams.get(source.slice(1));
			} catch (e) {
				return null;
			}
		}
		return null;
	};
	gsurfOnDom(function () {
		document.querySelectorAll(selector).forEach(function (el) {
			const v = extract(el);
			if (v && /^https?:\/\//.test(v)) {
				try {
					el.setAttribute('href', new URL(v).href);
				} catch (e) {
				}
			}
		});
	}, 'stay');
}

/// scriptlet nobab bab-defuser nofab fuckadblock.js-3.2.0
function noBab() {
	const F = function () {};
	F.prototype.check = function () { return this; };
	F.prototype.emitEvent = function () { return this; };
	F.prototype.clearEvent = function () {};
	F.prototype.on = function (detected, fn) {
		if (!detected) {
			try { fn(); } catch (e) {}
		}
		return this;
	};
	F.prototype.onDetected = function () { return this; };
	F.prototype.onNotDetected = function (fn) {
		try { fn(); } catch (e) {}
		return this;
	};
	F.prototype.setOption = function () { return this; };
	const o = new F();
	window.BlockAdBlock = window.FuckAdBlock = window.SniffAdBlock = F;
	window.blockAdBlock = window.fuckAdBlock = window.sniffAdBlock = o;
}

/// scriptlet noop
function gsurfNoop() {
}
