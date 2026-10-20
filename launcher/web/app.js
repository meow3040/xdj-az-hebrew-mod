// SPDX-License-Identifier: GPL-3.0-only
// The launcher's page: draws the state app.py reports (polled), and starts its jobs.
'use strict';

const TOKEN = location.hash.slice(1);
const $ = (sel, el = document) => el.querySelector(sel);
let G = null; // the deck's Hebrew glyphs (app.py /api/glyphs, from heb_glyphs.h)
let S = null; // the last state
let picked = null; // the stick chosen on this page
let ipTouched = false;
let restoreDeckNext = false;

async function api(path, body) {
    const opt = { headers: { Authorization: 'Bearer ' + TOKEN } };
    if (body !== undefined) {
        opt.method = 'POST';
        opt.headers['Content-Type'] = 'application/json';
        opt.body = JSON.stringify(body);
    }
    const r = await fetch(path, opt);
    if (r.status === 403) throw new Error('token');
    return r.json();
}

// ---- the deck's letters: Hebrew-only text, right to left, as the deck will draw it
function hebSvg(text, cls) {
    const upm = G.upm, space = upm * 0.28, asc = upm * 0.78, desc = upm * 0.26;
    let x = 0, paths = '';
    for (const ch of [...text].reverse()) {
        const g = G.glyphs[ch.codePointAt(0)];
        if (!g) { x += space; continue; }
        if (g.d) paths += `<path transform="translate(${x} 0)" d="${g.d}"/>`;
        x += g.adv;
    }
    return `<svg class="${cls}" viewBox="0 ${-asc} ${x} ${asc + desc}" aria-hidden="true">` +
        `<g transform="scale(1 -1)">${paths}</g></svg>`;
}
// what the deck draws today: a box for every letter it has no glyph for
function boxSvg(text) {
    const upm = G.upm, asc = upm * 0.78, desc = upm * 0.26, w = upm * 0.5, gap = upm * 0.14;
    let x = 0, rects = '';
    for (const ch of [...text]) {
        if (ch === ' ') { x += upm * 0.28; continue; }
        rects += `<rect x="${x + gap / 2}" y="${-upm * 0.64}" width="${w}" height="${upm * 0.64}"/>`;
        x += w + gap;
    }
    return `<svg class="boxes" viewBox="0 ${-asc} ${x} ${asc + desc}" aria-hidden="true">${rects}</svg>`;
}

const ROWS = [
    ['לילה לבן', 'הלהקה', 124],
    ['קיץ בתל אביב', 'דנה ורון', 128],
    ['אהבה ראשונה', 'עומר', 122],
    ['ריקוד אחרון', 'הדיג׳יי', 126],
    ['שמש של צהריים', 'מאיה', 120],
];

function drawDeck() {
    $('#title').insertAdjacentHTML('beforeend', hebSvg('עברית על הדק', 'h1'));
    const lcd = $('#lcd');
    ROWS.forEach(([name, artist, bpm], i) => {
        const li = document.createElement('li');
        if (i === 1) li.className = 'cursor';
        li.innerHTML = `<span class="art"></span>` +
            `<span class="name">${boxSvg(name)}${hebSvg(name, 'letters')}</span>` +
            `<span class="name artist">${boxSvg(artist)}${hebSvg(artist, 'letters')}</span>` +
            `<span class="bpm">${bpm}.00</span>`;
        lcd.appendChild(li);
    });
    // the one moment: the boxes turn into letters, row by row
    const still = matchMedia('(prefers-reduced-motion: reduce)').matches;
    [...lcd.children].forEach((li, i) => {
        if (still) li.classList.add('on');
        else setTimeout(() => li.classList.add('on'), 700 + i * 150);
    });
}

// ---- words
const BUILD_STEPS = {
    '1': 'מוריד את כלי הבנייה. בפעם הראשונה זה כ-70MB.',
    '2': 'פותח את כלי הבנייה. רק בפעם הראשונה.',
    '3': 'מכין את הקוד.',
    '4': 'מקמפל את azmod.so. זה לוקח כחצי דקה.',
    '5': 'בודק את התוצאה.',
};
const DECK_STEPS = {
    connect: 'מתחבר לדק.',
    stage: 'הדק טוען את המוד מהדיסק.',
    restart: 'הדק מופעל מחדש.',
    verify: 'בודק שהעברית עלתה.',
};
const ERR = {
    'no-wsl': () => 'לבנייה צריך את WSL של Windows: פותחים PowerShell כמנהל, מריצים wsl --install ומפעילים מחדש את המחשב.',
    'no-official': e => `לא נמצא במחשב AZ-MODS ${e.version} מאומת. מתקינים אותו פעם אחת דרך אפליקציית AZ-MODS.`,
    'build': e => `הבנייה נעצרה: ${e.text || 'שגיאה'}. הפירוט תחת "פרטים".`,
    'no-build': () => 'קודם לבנות את המוד, בצעד 1.',
    'stick-version': e => `על הדיסק AZ-MODS ${e.have || 'אחר'}, והמוד נבנה ל-${e.want}. מעדכנים את הדיסק באפליקציית AZ-MODS.`,
    'copy-verify': () => 'הקובץ לא נשמר שלם בדיסק. לנסות שוב, או בדיסק אחר.',
    'bad-ip': () => 'זו לא כתובת IP. כתובת נראית כך: 192.168.1.50.',
    'denied': () => 'הדק עוד לא אישר את המחשב. מדליקים את הדק כשהדיסק ב-USB 1, וכשהוא שואל Allow remote access בוחרים Allow.',
    'unreachable': () => 'אין תשובה מהכתובת הזו. לבדוק שהדק דלוק, מחובר לאותה רשת כמו המחשב, ושהדיסק ב-USB 1.',
    'hostkey': () => 'זהות הדק השתנתה מאז החיבור הקודם. זה קורה אחרי עדכון קושחה.',
    'ssh': e => `החיבור לדק נכשל. ${e.text || ''}`,
    'deck-not-azmods': () => 'בכתובת הזו אין דק עם AZ-MODS.',
    'deck-stage-timeout': () => 'הדק לא מצא את המוד. לבדוק שהדיסק שהעתקנו אליו הוא זה שב-USB 1.',
    'deck-settings-readonly': () => 'הדק לא הצליח לשמור את ההגדרה. להפעיל אותו מחדש ולנסות שוב.',
    'deck-timeout': () => 'הדק לא סיים בזמן. אפשר לנסות שוב.',
    'internal': e => `שגיאה בלאנצ׳ר: ${e.text}. הפירוט תחת "פרטים".`,
};
const errText = e => (ERR[e && e.code] || ERR.internal)(e || {});

function when(t) {
    const d = new Date(t * 1000), now = new Date();
    const hm = d.toLocaleTimeString('he-IL', { hour: '2-digit', minute: '2-digit' });
    return d.toDateString() === now.toDateString() ? `היום ב-${hm}` : `ב-${d.toLocaleDateString('he-IL')}`;
}

// ---- one step's look: its LED, its sentence, its pad
function paint(id, o) {
    const el = $('#' + id);
    el.className = 'step ' + (o.led || '') + (o.locked ? ' locked' : '');
    const st = $('.status', el);
    st.textContent = o.text || '';
    st.className = 'status ' + (o.tone || '');
    const pad = $('.pad', el);
    pad.textContent = o.pad || '';
    pad.hidden = !o.pad;
    pad.disabled = !!o.disabled || !!o.locked || !o.action;
    pad.classList.toggle('quiet', !!o.quiet);
    pad.dataset.action = o.action || '';
    const extra = $('.extra', el);
    extra.replaceChildren(...(o.extra || []));
}
function linkButton(text, fn) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'link';
    b.textContent = text;
    b.addEventListener('click', fn);
    return b;
}
const openApp = () => linkButton('פתיחת אפליקציית AZ-MODS', () => api('/api/open-app', {}));

function selectedStick() {
    const all = S.sticks;
    return all.find(s => s.letter === picked) || all.find(s => s.letter === S.stick) ||
        all.find(s => s.azmods) || all[0] || null;
}

function render() {
    const s = S, job = s.job, running = !!(job && job.state === 'running');
    const is = (kind, state) => job && job.kind === kind && job.state === state;
    $('#version').textContent = `נבנה מהקוד שבתיקייה הזו, על בסיס AZ-MODS ${s.version}.`;

    // 1. build
    const b = s.build;
    let o;
    if (is('build', 'running')) o = { led: 'busy', text: BUILD_STEPS[job.step] || 'מתחיל.', pad: 'בונה' };
    else if (!s.wsl) o = { led: 'attn', tone: 'warn', text: errText({ code: 'no-wsl' }), pad: 'לבנות' };
    else if (!s.official) o = { led: 'attn', tone: 'warn', text: errText({ code: 'no-official', version: s.version }), pad: 'לבנות', extra: s.app ? [openApp()] : [] };
    else if (is('build', 'error')) o = { led: 'bad', tone: 'bad', text: errText(job.error), pad: 'לנסות שוב', action: 'build' };
    else if (b && b.current) o = { led: 'done', tone: 'good', text: `המוד מוכן. נבנה ${when(b.time)}.`, pad: 'לבנות שוב', action: 'build', quiet: true };
    else if (b) o = { led: 'attn', tone: 'warn', text: 'הקוד השתנה מאז הבנייה האחרונה, אז בונים שוב.', pad: 'לבנות', action: 'build' };
    else o = { led: 'ready', text: 'בונים את azmod.so מהקוד שבתיקייה, יחד עם החלקים הרשמיים של AZ-MODS. בפעם הראשונה יורדים כ-70MB של כלי בנייה.', pad: 'לבנות', action: 'build' };
    if (running && !is('build', 'running')) o.disabled = true;
    paint('step-build', o);
    const built = !!(b && b.current);

    // 2. the stick
    const sel = selectedStick();
    const chips = $('#step-stick .chips');
    chips.replaceChildren(...s.sticks.map(st => {
        const c = document.createElement('button');
        c.type = 'button';
        c.className = 'chip';
        c.setAttribute('role', 'radio');
        c.setAttribute('aria-checked', String(sel && st.letter === sel.letter));
        c.innerHTML = `${escapeHtml(st.label || 'דיסק')} <bdi dir="ltr">${st.letter}:</bdi>` +
            `<small>${st.azmods ? 'AZ-MODS ' + escapeHtml(st.azmods) : 'בלי AZ-MODS'}, ${st.gb}GB</small>`;
        c.addEventListener('click', () => { picked = st.letter; api('/api/settings', { stick: st.letter }); render(); });
        return c;
    }));
    const onStick = !!(sel && sel.has === 'ours' && sel.ssh);
    if (!built) o = { locked: true, text: 'אחרי הבנייה.', pad: 'להעתיק לדיסק' };
    else if (is('install', 'running')) o = { led: 'busy', text: 'מעתיק לדיסק.', pad: 'מעתיק' };
    else if (!sel) o = { led: 'attn', tone: 'warn', text: 'לחבר למחשב את הדיסק-און-קי של הדק.', pad: 'להעתיק לדיסק' };
    else if (!sel.azmods) o = { led: 'attn', tone: 'warn', text: 'אין על הדיסק הזה AZ-MODS. מתקינים אותו עליו באפליקציית AZ-MODS, וחוזרים לכאן.', pad: 'להעתיק לדיסק', extra: s.app ? [openApp()] : [] };
    else if (sel.azmods !== s.version) o = { led: 'attn', tone: 'warn', text: errText({ code: 'stick-version', have: sel.azmods, want: s.version }), pad: 'להעתיק לדיסק', extra: s.app ? [openApp()] : [] };
    else if (is('install', 'error')) o = { led: 'bad', tone: 'bad', text: errText(job.error), pad: 'לנסות שוב', action: 'install' };
    else if (is('restore-stick', 'running')) o = { led: 'busy', text: 'מחזיר לדיסק את azmod.so הרשמי.', pad: 'להעתיק לדיסק' };
    else if (sel.has === 'official' && job && /^restore/.test(job.kind) && job.state !== 'error') o = { led: 'ready', text: 'azmod.so הרשמי חזר לדיסק. אפשר להעתיק את מוד העברית שוב בכל רגע.', pad: 'להעתיק לדיסק', action: 'install' };
    else if (onStick) o = { led: 'done', tone: 'good', text: 'מוד העברית נמצא על הדיסק.', pad: 'להעתיק שוב', action: 'install', quiet: true };
    else o = { led: 'ready', text: 'azmod.so שעל הדיסק יוחלף במוד העברית, ויתווסף מפתח שמאפשר למחשב להתחבר לדק. העותק הרשמי נשאר במחשב.', pad: 'להעתיק לדיסק', action: 'install' };
    if (sel && sel.firmware && !['2.00', '2.01'].includes(sel.firmware) && !o.locked) {
        const note = document.createElement('p');
        note.className = 'status warn';
        note.textContent = `הדק על קושחה ${sel.firmware}: האותיות העבריות יעבדו, ימין-לשמאל והמקלדת צריכים 2.01.`;
        o.extra = (o.extra || []).concat(note);
    }
    if (running && !is('install', 'running')) o.disabled = true;
    paint('step-stick', o);

    // 3. the deck
    const ip = $('#ip');
    if (!ipTouched && document.activeElement !== ip && ip.value !== (s.deck_ip || '')) ip.value = s.deck_ip || '';
    const ipOk = /^\d{1,3}(\.\d{1,3}){3}$/.test(ip.value.trim());
    const found = $('#step-deck .found');
    const deckLocked = !onStick && !s.deck_done;
    if (is('restore-deck', 'running')) o = { led: 'busy', text: 'מחזיר את הדק ל-AZ-MODS הרשמי.', pad: 'להפעיל עברית' };
    else if (is('restore-deck', 'error')) o = { led: 'bad', tone: 'bad', text: errText(job.error), pad: 'להפעיל עברית' };
    else if (is('restore-deck', 'ok')) o = { locked: true, text: 'הדק חזר ל-AZ-MODS הרשמי, בלי מוד העברית.', pad: 'להפעיל עברית' };
    else if (deckLocked) o = { locked: true, text: 'אחרי שהמוד על הדיסק.', pad: 'להפעיל עברית' };
    else if (is('find', 'running')) o = { led: 'busy', text: 'מחפש את הדק ברשת.', pad: 'להפעיל עברית' };
    else if (is('apply', 'running')) o = { led: 'busy', text: DECK_STEPS[job.step] || 'מתחיל.', pad: 'מפעיל' };
    else if (is('apply', 'error')) o = { led: 'bad', tone: 'bad', text: errText(job.error), pad: 'לנסות שוב', action: 'apply', extra: job.error.code === 'hostkey' ? [linkButton('לאשר את הדק מחדש', forgetHost)] : [] };
    else if (is('apply', 'ok') && job.result) o = applied(job.result.status || []);
    else if (s.deck_done === 'ok') o = { led: 'done', tone: 'good', text: 'עברית פועלת בדק. בשורה Hebrew ב-Mod Menu אפשר לכבות ולהדליק אותה.', pad: 'להפעיל שוב', action: 'apply', quiet: true };
    else o = { led: 'ready', text: '', pad: 'להפעיל עברית', action: 'apply' };
    if (!o.locked && o.action === 'apply' && !ipOk) o.disabled = true;
    if (is('find', 'ok') && job.result) {
        const f = job.result.found || [];
        if (!f.length && !o.text) { o.text = 'לא נמצא דק ברשת. לבדוק שהדק דלוק ומחובר ל-Wi-Fi, ושהמחשב באותה רשת.'; o.tone = 'warn'; }
        found.replaceChildren(...(f.length > 1 ? f.map(a => {
            const c = document.createElement('button');
            c.type = 'button';
            c.className = 'chip';
            c.setAttribute('role', 'listitem');
            c.dir = 'ltr';
            c.textContent = a;
            c.addEventListener('click', () => { ip.value = a; ipTouched = true; api('/api/settings', { deck_ip: a }); render(); });
            return c;
        }) : []));
    } else found.replaceChildren();
    if (running && !is('apply', 'running')) o.disabled = true;
    paint('step-deck', o);
    $('#find').disabled = deckLocked || running;
    ip.disabled = deckLocked;
    $('#step-deck .how').hidden = !!(o.led === 'done');

    for (const id of ['restore', 'folder']) $('#' + id).disabled = running;
    $('#restore').disabled = running || !s.official;
    const det = $('#details');
    if (!det.hidden) det.textContent = job ? job.lines.join('\n') : 'אין עדיין פעולה להציג.';

    // the restore's second half, once the stick has the official file again
    if (restoreDeckNext && is('restore-stick', 'ok')) {
        restoreDeckNext = false;
        if (/^\d{1,3}(\.\d{1,3}){3}$/.test(ip.value.trim()) && s.deck_done) api('/api/restore-deck', { ip: ip.value.trim() }).then(poll);
    }
}

function applied([kind, conf, kbd]) {
    if (kind === 'ok') {
        return { led: 'done', tone: 'good', pad: 'להפעיל שוב', action: 'apply', quiet: true,
            text: kbd === '6' ? 'עברית פועלת בדק, עם ימין-לשמאל ומקלדת עברית. בשורה Hebrew ב-Mod Menu אפשר לכבות ולהדליק אותה.'
                : 'עברית פועלת בדק. בשורה Hebrew ב-Mod Menu אפשר לכבות ולהדליק אותה.' };
    }
    if (kind === 'restored') return { led: 'ready', text: 'הדק חזר ל-AZ-MODS הרשמי.', pad: 'להפעיל עברית', action: 'apply' };
    if (conf === 'skip') return { led: 'attn', tone: 'warn', pad: 'להפעיל שוב', action: 'apply', text: 'המוד בדק. נשאר להדליק את Hebrew ב-Mod Menu ולבחור Reload.' };
    return { led: 'attn', tone: 'warn', pad: 'להפעיל שוב', action: 'apply', text: 'הדק עלה, אבל לא אושר שהעברית נטענה. כדאי לפתוח את Mod Menu ולבדוק את השורה Hebrew.' };
}

function escapeHtml(t) {
    return String(t).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

// ---- actions
function ask(title, text, yes) {
    return new Promise(resolve => {
        const d = $('#confirm');
        $('#confirm-title').textContent = title;
        $('#confirm-text').textContent = text;
        $('#confirm-yes').textContent = yes;
        d.addEventListener('close', () => resolve(d.returnValue === 'yes'), { once: true });
        d.returnValue = '';
        d.showModal();
    });
}

async function act(action) {
    const sel = selectedStick();
    const ip = $('#ip').value.trim();
    if (action === 'build') await api('/api/build', {});
    else if (action === 'install' && sel) await api('/api/install', { stick: sel.letter });
    else if (action === 'apply') {
        const ok = await ask('להפעיל את הדק מחדש?', 'הדק יכבה את התוכנה שלו לכמה שניות ויעלה עם העברית. שירים טעונים יורדו מהדקים, אז לא באמצע הופעה.', 'להפעיל מחדש');
        if (!ok) return;
        await api('/api/apply', { ip });
    }
    poll();
}

async function forgetHost() {
    await api('/api/forget-host', { ip: $('#ip').value.trim() });
    act('apply');
}

async function restore() {
    const sel = selectedStick();
    if (!sel || !sel.azmods) return;
    const deck = !!S.deck_done;
    const ok = await ask('לחזור ל-AZ-MODS הרשמי?',
        deck ? 'azmod.so הרשמי יחזור לדיסק, והדק יופעל מחדש בלי מוד העברית. אפשר לחזור לעברית בכל רגע.'
            : 'azmod.so הרשמי יחזור לדיסק. אפשר לחזור לעברית בכל רגע.', 'לחזור לרשמי');
    if (!ok) return;
    restoreDeckNext = deck;
    await api('/api/restore', { stick: sel.letter });
    poll();
}

let timer = null;
async function poll() {
    clearTimeout(timer);
    try {
        S = await api('/api/state');
        render();
        timer = setTimeout(poll, S.job && S.job.state === 'running' ? 700 : 1500);
    } catch (e) {
        document.body.classList.add('gone');
        $('#version').textContent = 'הלאנצ׳ר נסגר. פותחים אותו שוב מ-AZ-Hebrew-Launcher.cmd.';
        for (const b of document.querySelectorAll('button, input')) b.disabled = true;
    }
}

async function start() {
    for (const step of document.querySelectorAll('.step')) {
        $('.pad', step).addEventListener('click', e => act(e.currentTarget.dataset.action));
    }
    const ip = $('#ip');
    ip.addEventListener('input', () => { ipTouched = true; if (S) render(); });
    ip.addEventListener('change', () => api('/api/settings', { deck_ip: ip.value.trim() }));
    $('#find').addEventListener('click', async () => { ipTouched = false; await api('/api/find', {}); poll(); });
    $('#restore').addEventListener('click', restore);
    $('#folder').addEventListener('click', () => api('/api/open-folder', {}));
    $('#details-toggle').addEventListener('click', e => {
        const det = $('#details');
        det.hidden = !det.hidden;
        e.currentTarget.setAttribute('aria-expanded', String(!det.hidden));
        if (S) render();
    });
    $('#quit').addEventListener('click', async () => {
        try { await api('/api/quit', {}); } catch (e) { /* already gone */ }
        window.close();
    });
    try {
        G = await api('/api/glyphs');
        drawDeck();
    } catch (e) { /* the page still works without the drawing */ }
    poll();
}
start();
