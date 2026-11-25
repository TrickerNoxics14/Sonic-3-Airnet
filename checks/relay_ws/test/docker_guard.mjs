// The guard behind the "container image" test: which files does the program need, and does the Dockerfile copy them?
// It must not pass just because it did not understand something, so every form it cannot follow is reported in `unknown` and the test fails on it.
// What it follows: static imports and re-exports ("from './x.mjs'", import './x.mjs'), dynamic import('./x.mjs'), single or double quotes, sub folders.
// What it reports as unknown: an import that leaves the folder ('../'), without extension, of a file that does not exist (also a comment or a string that only looks like
// an import), import(variable), require(...), createRequire, new URL('./x', import.meta.url); in the Dockerfile: COPY --from=..., JSON-array COPY, a .dockerignore with "!" lines.
import path from 'node:path';

const posix = path.posix;

/** Reads the relative imports of `entry` (a path inside the folder, '/' separated) and of everything it imports. read(name) returns the text or null. */
export function importedFiles(entry, read) {
	const files = new Set();
	const unknown = [];
	const visit = (name) => {
		if (files.has(name)) return;
		files.add(name);
		const text = read(name);
		if (text === null) { unknown.push(`${name}: file not found`); return; }
		for (const match of text.matchAll(/\b(?:from|import)\s*\(?\s*(['"])([^'"\r\n]+)\1/g)) {
			const specifier = match[2];
			if (!specifier.startsWith('./') && !specifier.startsWith('../')) continue;   // a node: module or a package
			const target = posix.normalize(posix.join(posix.dirname(name), specifier));
			if (target.startsWith('../') || target === '..') { unknown.push(`${name}: "${specifier}" leaves the folder, the image cannot hold it`); continue; }
			if (!posix.extname(target)) { unknown.push(`${name}: "${specifier}" has no file extension (Node needs one for ES modules)`); continue; }
			if (read(target) === null) { unknown.push(`${name}: "${specifier}" is no file in the folder (a comment? a computed name?)`); continue; }
			visit(target);
		}
		if (/\bimport\s*\(\s*(?!['"])/.test(text)) unknown.push(`${name}: import(...) of something that is not a plain string`);
		if (/\brequire\s*\(|\bcreateRequire\b/.test(text)) unknown.push(`${name}: require / createRequire`);
		if (/new\s+URL\s*\(\s*['"]\.{1,2}\//.test(text)) unknown.push(`${name}: a file read by URL relative to the module`);
	};
	visit(entry);
	return { files: [...files].sort(), unknown };
}

/** The sources of the COPY / ADD lines of a Dockerfile. */
export function dockerSources(text) {
	const sources = [];
	const unknown = [];
	const joined = text.replace(/\\\r?\n/g, ' ');
	for (const raw of joined.split(/\r?\n/)) {
		const line = raw.trim();
		const instruction = /^(COPY|ADD)\s+(.*)$/i.exec(line);
		if (!instruction) continue;
		const rest = instruction[2].trim();
		if (rest.startsWith('[')) { unknown.push(`${line}: the JSON form of ${instruction[1]} is not followed`); continue; }
		const words = rest.split(/\s+/);
		const flags = words.filter((w) => w.startsWith('--'));
		if (flags.some((f) => /^--from=/i.test(f))) { unknown.push(`${line}: --from= copies from another stage, not from this folder`); continue; }
		const paths = words.filter((w) => !w.startsWith('--'));
		sources.push(...paths.slice(0, -1));
	}
	return { sources, unknown };
}

export function globToRegExp(glob) {
	const escaped = glob.replace(/[.+^${}()|[\]\\]/g, '\\$&');
	return new RegExp(`^${escaped.replace(/\*\*/g, '\u0000').replace(/\*/g, '[^/]*').replace(/\?/g, '[^/]').replace(/\u0000/g, '.*')}$`);
}

const clean = (p) => p.replace(/^\.\//, '').replace(/\/+$/, '');

/** True if `file` (relative path, '/' separated) is copied: matched by a source glob, inside a copied folder, or everything is copied ('.') and the file is not in .dockerignore. */
export function isCopied(file, sources, dockerignoreText = '') {
	const ignoreLines = dockerignoreText.split(/\r?\n/).map((l) => l.trim()).filter((l) => l && !l.startsWith('#'));
	const ignored = ignoreLines.some((pattern) => {
		const p = clean(pattern);
		return globToRegExp(p).test(file) || globToRegExp(`${p}/**`).test(file) || globToRegExp(`**/${p}`).test(file);
	});
	return sources.some((source) => {
		const s = clean(source);
		if (s === '.' || s === '') return !ignored;
		if (ignored) return false;
		return globToRegExp(s).test(file) || globToRegExp(`${s}/**`).test(file);
	});
}

/** The files of the image: everything in `files` that the sources copy. */
export function imageFiles(files, sources, dockerignoreText = '') {
	return files.filter((file) => isCopied(file, sources, dockerignoreText));
}

/** What the guard finds wrong: unknown forms, and files the program needs that no COPY brings. */
export function checkImage({ entry, read, dockerfile, dockerignore = '', extraNeeded = ['package.json'] }) {
	const needed = importedFiles(entry, read);
	const copies = dockerSources(dockerfile);
	const problems = [...needed.unknown, ...copies.unknown];
	if (ignoreHasNegation(dockerignore)) problems.push('.dockerignore has "!" lines, not followed');
	for (const file of [...needed.files, ...extraNeeded]) if (!isCopied(file, copies.sources, dockerignore)) problems.push(`${file}: needed by the program but not copied by the Dockerfile`);
	return { problems, needed: needed.files, sources: copies.sources };
}

function ignoreHasNegation(text) { return text.split(/\r?\n/).some((l) => l.trim().startsWith('!')); }
