// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Zach Podbielniak
//
// gsurf adblock procedural cosmetic engine.
//
// Runs in an isolated script world: it shares the DOM with the page but
// none of its JavaScript, so the page can neither see nor tamper with it.
// The web-process extension evaluates this file in every frame and then
// calls gsurfAdblockCosmetic([...selectors]) with the filters that apply
// to the frame's host. Plain selectors are hidden natively by WebKit's
// content blocker; what arrives here needs code:
//
//   procedural operators  :has-text() :-abp-contains() :upward() :xpath()
//                         :matches-css() :min-text-length() :matches-attr()
//                         :matches-path() :matches-media() :if() :if-not()
//                         and :has() / :not() with a procedural argument
//   actions               :style(decl) :remove() :remove-attr() :remove-class()
//   (and plain selectors whose filter has negated subdomains)

'use strict';

const gsurfAdblockOps = new Set([
	'has-text', '-abp-contains', 'contains', 'upward', 'nth-ancestor', 'xpath',
	'matches-css', 'matches-css-before', 'matches-css-after', 'min-text-length',
	'matches-attr', 'matches-path', 'matches-media', 'watch-attr', 'if', 'if-not',
	'has', '-abp-has', 'not', 'others', 'shadow', 'matches-prop',
	'style', 'remove', 'remove-attr', 'remove-class',
]);
const gsurfAdblockActions = new Set([ 'style', 'remove', 'remove-attr', 'remove-class' ]);
const gsurfAdblockProceduralRe = /:(has-text|-abp-contains|contains|upward|nth-ancestor|xpath|matches-css(-before|-after)?|min-text-length|matches-attr|matches-path|matches-media|watch-attr|if|if-not|others|shadow|matches-prop|style|remove|remove-attr|remove-class)\(/;

// "/re/flags" or a literal string -> RegExp.
function gsurfAdblockPattern(text, exact) {
	const m = /^\/(.+)\/([imsu]*)$/.exec(text);
	if (m !== null) {
		return new RegExp(m[1], m[2]);
	}
	const escaped = text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
	return new RegExp(exact ? '^' + escaped + '$' : escaped);
}

function gsurfAdblockUnquote(text) {
	text = text.trim();
	if (/^(["']).*\1$/.test(text)) {
		return text.slice(1, -1);
	}
	return text;
}

// Split a selector into tasks: [['css', s] | ['spath', s] | ['op', name, arg]]
// plus a trailing action. Returns null for anything unparseable.
function gsurfAdblockParse(selector) {
	const tasks = [];
	let action = [ 'hide', '' ];
	let css = '';
	let i = 0;
	const flush = function () {
		if (css.trim() !== '' || (css !== '' && tasks.length > 0)) {
			tasks.push([ tasks.length === 0 ? 'css' : 'spath', css ]);
		}
		css = '';
	};
	while (i < selector.length) {
		const c = selector[i];
		if (c === '"' || c === "'") {
			const end = selector.indexOf(c, i + 1);
			if (end === -1) {
				return null;
			}
			css += selector.slice(i, end + 1);
			i = end + 1;
			continue;
		}
		if (c === '[') {
			const end = selector.indexOf(']', i);
			if (end === -1) {
				return null;
			}
			css += selector.slice(i, end + 1);
			i = end + 1;
			continue;
		}
		if (c === ':') {
			const m = /^:([-a-z]+)\(/.exec(selector.slice(i));
			if (m !== null && gsurfAdblockOps.has(m[1])) {
				// Find the balanced closing parenthesis.
				let depth = 0;
				let j = i + m[0].length - 1;
				let quote = null;
				for (; j < selector.length; j++) {
					const d = selector[j];
					if (quote !== null) {
						if (d === '\\') {
							j++;
						} else if (d === quote) {
							quote = null;
						}
						continue;
					}
					if (d === '"' || d === "'") {
						quote = d;
					} else if (d === '(') {
						depth++;
					} else if (d === ')' && --depth === 0) {
						break;
					}
				}
				if (j >= selector.length) {
					return null;
				}
				const name = m[1];
				const arg = selector.slice(i + m[0].length, j);
				// :has()/:not() with a plain argument is native CSS.
				if ((name === 'has' || name === 'not') && !gsurfAdblockProceduralRe.test(arg)) {
					css += selector.slice(i, j + 1);
					i = j + 1;
					continue;
				}
				if (gsurfAdblockActions.has(name)) {
					if (selector.slice(j + 1).trim() !== '') {
						return null;
					}
					flush();
					action = [ name, arg ];
					break;
				}
				flush();
				tasks.push([ 'op', name, arg ]);
				i = j + 1;
				continue;
			}
		}
		css += c;
		i++;
	}
	flush();
	return { tasks: tasks, action: action };
}

function gsurfAdblockUnique(nodes) {
	return Array.from(new Set(nodes));
}

// Query relative to an element (":scope" makes leading combinators work).
function gsurfAdblockQuery(root, css) {
	if (root === document || root === null) {
		return Array.from(document.querySelectorAll(css));
	}
	const t = css.trim();
	const scoped = /^[>+~]/.test(t) ? ':scope ' + t : ':scope ' + t;
	return Array.from(root.querySelectorAll(scoped));
}

function gsurfAdblockRun(tasks, roots) {
	let nodes = roots;
	for (const task of tasks) {
		if (nodes.length === 0) {
			break;
		}
		if (task[0] === 'css') {
			nodes = gsurfAdblockUnique(nodes.flatMap(function (n) {
				return gsurfAdblockQuery(n, task[1]);
			}));
			continue;
		}
		if (task[0] === 'spath') {
			const s = task[1];
			if (/^\s|^[>+~]/.test(s)) {
				nodes = gsurfAdblockUnique(nodes.flatMap(function (n) {
					return gsurfAdblockQuery(n, s);
				}));
			} else {
				nodes = nodes.filter(function (n) {
					return n.matches !== undefined && n.matches(s);
				});
			}
			continue;
		}
		nodes = gsurfAdblockOp(task[1], task[2], nodes);
	}
	return nodes.filter(function (n) { return n instanceof Element; });
}

function gsurfAdblockSelfMatches(el, arg) {
	const parsed = gsurfAdblockParse(arg);
	if (parsed === null) {
		return false;
	}
	return gsurfAdblockRun(parsed.tasks, [ el.parentElement || document ]).includes(el);
}

function gsurfAdblockOp(name, arg, nodes) {
	switch (name) {
	case 'has-text':
	case '-abp-contains':
	case 'contains': {
		const re = gsurfAdblockPattern(gsurfAdblockUnquote(arg));
		return nodes.filter(function (n) { return re.test(n.textContent); });
	}
	case 'min-text-length': {
		const min = parseInt(arg, 10);
		return nodes.filter(function (n) { return n.textContent.length >= min; });
	}
	case 'upward':
	case 'nth-ancestor': {
		const steps = /^\d+$/.test(arg.trim()) ? parseInt(arg, 10) : NaN;
		return gsurfAdblockUnique(nodes.map(function (n) {
			if (isNaN(steps)) {
				return n.parentElement !== null ? n.parentElement.closest(arg) : null;
			}
			let p = n;
			for (let k = 0; k < steps && p !== null; k++) {
				p = p.parentElement;
			}
			return p;
		}).filter(Boolean));
	}
	case 'xpath': {
		const out = [];
		for (const n of nodes) {
			const result = document.evaluate(arg, n, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
			for (let k = 0; k < result.snapshotLength; k++) {
				out.push(result.snapshotItem(k));
			}
		}
		return gsurfAdblockUnique(out);
	}
	case 'matches-css':
	case 'matches-css-before':
	case 'matches-css-after': {
		const colon = arg.indexOf(':');
		if (colon === -1) {
			return [];
		}
		const prop = arg.slice(0, colon).trim();
		const re = gsurfAdblockPattern(arg.slice(colon + 1).trim(), true);
		const pseudo = name === 'matches-css' ? null : '::' + name.slice(12);
		return nodes.filter(function (n) {
			return re.test(window.getComputedStyle(n, pseudo).getPropertyValue(prop));
		});
	}
	case 'matches-attr': {
		const eq = arg.indexOf('=');
		const nameRe = gsurfAdblockPattern(gsurfAdblockUnquote(eq === -1 ? arg : arg.slice(0, eq)), true);
		const valueRe = eq === -1 ? /^/ : gsurfAdblockPattern(gsurfAdblockUnquote(arg.slice(eq + 1)), true);
		return nodes.filter(function (n) {
			return Array.from(n.attributes || []).some(function (a) {
				return nameRe.test(a.name) && valueRe.test(a.value);
			});
		});
	}
	case 'matches-path':
		return gsurfAdblockPattern(arg).test(location.pathname + location.search) ? nodes : [];
	case 'matches-media':
		return window.matchMedia(arg).matches ? nodes : [];
	case 'watch-attr':
		return nodes;
	case 'has':
	case '-abp-has':
	case 'if': {
		const parsed = gsurfAdblockParse(arg);
		if (parsed === null) {
			return [];
		}
		return nodes.filter(function (n) {
			return gsurfAdblockRun(parsed.tasks, [ n ]).length > 0;
		});
	}
	case 'if-not': {
		const parsed = gsurfAdblockParse(arg);
		if (parsed === null) {
			return [];
		}
		return nodes.filter(function (n) {
			return gsurfAdblockRun(parsed.tasks, [ n ]).length === 0;
		});
	}
	case 'not':
		return nodes.filter(function (n) { return !gsurfAdblockSelfMatches(n, arg); });
	default:
		/* :others(), :shadow(), :matches-prop(): not supported. */
		return [];
	}
}

function gsurfAdblockAct(el, action) {
	switch (action[0]) {
	case 'hide':
		el.style.setProperty('display', 'none', 'important');
		break;
	case 'remove':
		el.remove();
		break;
	case 'style':
		// No url(): a style filter must not make the page fetch anything.
		if (/url\s*\(/i.test(action[1])) {
			break;
		}
		action[1].split(';').forEach(function (decl) {
			const colon = decl.indexOf(':');
			if (colon > 0) {
				const value = decl.slice(colon + 1).replace(/!\s*important\s*$/i, '').trim();
				el.style.setProperty(decl.slice(0, colon).trim(), value, 'important');
			}
		});
		break;
	case 'remove-attr': {
		const re = gsurfAdblockPattern(gsurfAdblockUnquote(action[1]), true);
		Array.from(el.attributes).forEach(function (a) {
			if (re.test(a.name)) {
				el.removeAttribute(a.name);
			}
		});
		break;
	}
	case 'remove-class': {
		const re = gsurfAdblockPattern(gsurfAdblockUnquote(action[1]), true);
		Array.from(el.classList).forEach(function (c) {
			if (re.test(c)) {
				el.classList.remove(c);
			}
		});
		break;
	}
	}
}

// Entry point: apply @selectors now, when the DOM is ready, and again
// (rate-limited) whenever the document changes.
function gsurfAdblockCosmetic(selectors) {
	const rules = selectors.map(gsurfAdblockParse).filter(Boolean);
	if (rules.length === 0) {
		return;
	}
	const apply = function () {
		for (const rule of rules) {
			let nodes;
			try {
				nodes = gsurfAdblockRun(rule.tasks, [ document ]);
			} catch (e) {
				continue;
			}
			for (const n of nodes) {
				try {
					gsurfAdblockAct(n, rule.action);
				} catch (e) {
				}
			}
		}
	};
	let timer = null;
	let last = 0;
	const schedule = function () {
		if (timer !== null) {
			return;
		}
		const wait = Math.max(0, 100 - (Date.now() - last));
		timer = setTimeout(function () {
			timer = null;
			last = Date.now();
			apply();
		}, wait);
	};
	if (document.readyState === 'loading') {
		document.addEventListener('DOMContentLoaded', apply, { once: true });
	} else {
		apply();
	}
	new MutationObserver(schedule).observe(document, { childList: true, subtree: true });
}
