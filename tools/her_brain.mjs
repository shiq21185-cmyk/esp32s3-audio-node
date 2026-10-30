// ⚠️ 2026-10-28 起：**身体已经不调用这个脚本了**。
//   真链路是 tools/body_talk.py 的 ask_her() → POST 桥接 /api/body/turn，
//   直接投进她 QQ 私聊那条会话 —— 这样她在板子上和聊天窗口里是同一个她。
//   这个脚本走的是**另开一段独立会话**的老路子（.tmp/body-session.json），
//   只在想单独试验 DSH 而不打扰她时才有用，别拿它当身体的脑子。
//
// 把一句话送进小鲸鱼的脑子（DSH agent），把她的回复文本拿回来。
//
//   node tools/her_brain.mjs "你好呀"              → 打印她的回复（纯文本到 stdout）
//   node tools/her_brain.mjs "你好呀" --new        → 丢掉之前的身体会话，重开一段
//   node tools/her_brain.mjs --session            → 只打印当前用的 sessionId
//
// 设计要点：
//   · stdout 只放**她的回复正文**，其余一律走 stderr —— 方便 python 那边直接当字符串用。
//   · 会话 id 存在 .tmp/body-session.json 里，下一次调用接着用，
//     这样她跟你在身体里说话是有上下文的，不是每句都失忆。
//   · 用的是她自己的 agent preset（config.json 的 agentPreset = qq-chat），
//     所以人格、记忆方式、说话习惯都和 QQ 里那个她是同一个。
//     ⚠️ 这是一段**独立的会话**，不带 QQ 那边的聊天记录；QQ 里的她自己不受影响。
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROT = path.resolve(__dirname, '..');
const BRIDGE = 'C:\\Users\\Administrator\\Desktop\\QQ bot\\qq-bridge-deploy';
const { NodeApiClient, unwrap, createTurnCollector, discoverDshLaunchToken } =
  await import('file:///' + BRIDGE.replace(/\\/g, '/') + '/src/dsh-client.js');

const STATE = path.join(ROT, '.tmp', 'body-session.json');
const CWD = path.join(ROT, '.tmp', 'body-session');

function log(msg) { process.stderr.write(msg + '\n'); }

function readCfg() {
  try { return JSON.parse(fs.readFileSync(path.join(BRIDGE, 'config.json'), 'utf8')); }
  catch { return {}; }
}

function authOf() {
  const cfg = readCfg();
  return {
    token: cfg.dsh?.authToken || discoverDshLaunchToken(),
    header: cfg.dsh?.authHeader,
    prefix: cfg.dsh?.authPrefix,
  };
}

function baseUrlOf() {
  return readCfg().dsh?.baseUrl || 'http://127.0.0.1:19387';
}

function presetOf() {
  const cfg = readCfg();
  return cfg.socialV2?.agentPreset || cfg.agentPreset || 'qq-chat';
}

function loadState() {
  try { return JSON.parse(fs.readFileSync(STATE, 'utf8')); } catch { return {}; }
}

function saveState(s) {
  fs.mkdirSync(path.dirname(STATE), { recursive: true });
  fs.writeFileSync(STATE, JSON.stringify(s, null, 2), 'utf8');
}

/**
 * 身体模式的第一句话：只在新会话开头补一次。
 * 不补的话她会以为还在 QQ 上打字（会去查语音通话、会说你"打了字"），
 * 而且习惯性写一大段——一大段念出来要三四十秒，对话就没法进行了。
 */
function roleCard() {
  // 同一张人格卡：state/current-role.json 指定角色名，正文在 roles/<角色>.md。
  // 校验规则跟桥接那边一致（不许带路径分隔符、不许 ..），别让它变成任意文件读取。
  try {
    const st = JSON.parse(fs.readFileSync(path.join(BRIDGE, 'state', 'current-role.json'), 'utf8'));
    const name = String(st.role ?? '').replace(/[\\/:*?"<>|]/g, '').trim();
    if (!name || name.includes('..')) return '';
    const file = path.join(BRIDGE, 'roles', name + '.md');
    if (!fs.existsSync(file)) return '';
    // [SILENT] 是 QQ 一代模式的状态机指令，念出来会很怪，逐行滤掉。
    return fs.readFileSync(file, 'utf8')
      .split('\n')
      .filter((line) => !line.includes('[SILENT]'))
      .join('\n');
  } catch {
    return '';
  }
}

/**
 * 身体模式的第一句话：只在新会话开头补一次。
 * 两件事：
 *  ① 告诉她现在在什么环境里（不补的话她会以为还在 QQ 上打字：会去查语音通话、会说你"打了字"，
 *     而且习惯性写一大段 —— 一大段念出来要三四十秒，对话就没法进行了）。
 *  ② 把人格卡补上（不补的话她在板子上根本没有「她是谁」，会现编名字）。
 */
function bodyHint() {
  const card = roleCard();
  const head =
    '（身体模式，不是 QQ）\n\n' +
    '你现在在一个物理身体里：一台 ESP32-S3 的小机器，带一个麦克风、一个小喇叭。' +
    '我说的每句话都是我用嘴说的、被你的麦克风听见的；你回的话会被直接念出来，从你旁边的喇叭放出来。\n' +
    '所以像真人当面聊天那样说话：口语、短 —— 最多两句话、40 个字以内；' +
    '不要 Markdown、不要列清单、不要提"发消息""打字""群里"。\n' +
    '我每句话前面可能会加一个〔现在是 …〕，那是背景（你看不到钟），别把它念出来。\n\n';
  if (!card) return head;
  return head + '【你是谁】\n以下是你在 QQ 里用的那张人格卡 —— 你就是这个人。\n\n' + card + '\n\n（以上是你本人，不是别人在跟你说话。）\n\n';
}

/** 会话还在不在（DSH 重启或手动删了就要重开）。 */
async function sessionAlive(api, sessionId) {
  if (!sessionId) return false;
  try {
    const list = unwrap(await api.sessions.list({}), 'sessions.list');
    const arr = Array.isArray(list) ? list : (list?.sessions ?? []);
    // 列表是空的 = 这套 DSH 按工作区过滤了（我们的 cwd 在 .tmp 下，不在注册工作区里），
    // 属于"问不出来"，不代表会话死了。这时候沿用保存的 id，别每句都失忆。
    if (arr.length === 0) return true;
    return arr.some((s) => (s.sessionId ?? s.id) === sessionId);
  } catch {
    return false;
  }
}

async function ensureSession(api, { forceNew }) {
  const st = loadState();
  if (!forceNew && st.sessionId && await sessionAlive(api, st.sessionId)) {
    log(`♻️  沿用身体会话 ${st.sessionId}`);
    return { sessionId: st.sessionId, fresh: false };
  }
  fs.mkdirSync(CWD, { recursive: true });
  const preset = presetOf();
  let created;
  try {
    created = unwrap(await api.sessions.create({ cwd: CWD, agentPreset: preset }), 'session.create');
  } catch (e) {
    log(`⚠️  带 preset "${preset}" 创建失败（${e?.message ?? e}），退回默认 preset`);
    created = unwrap(await api.sessions.create({ cwd: CWD }), 'session.create');
  }
  const sessionId = created.sessionId ?? created.id;
  log(`🆕 建了身体会话 ${sessionId}（preset=${preset}）`);
  saveState({ sessionId, preset, createdAt: new Date().toISOString() });
  return { sessionId, fresh: true };
}


/** 从 assistant/chunk 的 data 里尽量扒出增量文本（形状先按常见的几种兜着，实际以日志为准）。 */
function chunkText(d) {
  if (!d) return '';
  if (typeof d === 'string') return d;
  if (typeof d.text === 'string') return d.text;
  if (typeof d.delta === 'string') return d.delta;
  if (d.delta && typeof d.delta.text === 'string') return d.delta.text;
  if (Array.isArray(d.content)) return d.content.filter((b) => b?.type === 'text' && typeof b.text === 'string').map((b) => b.text).join('');
  if (d.block && typeof d.block.text === 'string') return d.block.text;
  return '';
}

async function main() {
  const argv = process.argv.slice(2);
  const text = argv.filter((a) => !a.startsWith('--')).join(' ').trim();
  const forceNew = argv.includes('--new');
  const onlyId = argv.includes('--session');
  // --stream：把她的回复边生成边吐到 stdout（每行一段增量），最后一行给全文。
  // 语音那边据此可以一句话一句话地合成、播放，不用等整段写完。
  const streamMode = argv.includes('--stream');

  const api = new NodeApiClient(baseUrlOf(), undefined, authOf());
  unwrap(await api.settings.describe({}), 'settings.describe');

  const { sessionId, fresh } = await ensureSession(api, { forceNew });
  if (onlyId) { process.stdout.write(sessionId + '\n'); return; }
  if (!text) { log('用法：node tools/her_brain.mjs "要说的话" [--new]'); process.exit(2); }

  // 新会话的第一句带上身体模式说明（只在第一句，之后她记得）
  const payload = fresh ? bodyHint() + text : text;

  // 先开事件流、等 WebSocket 真连上，再 prompt —— 否则 turn 事件会漏。
  const collector = createTurnCollector();
  let sawChunk = false;
  let openResolve;
  const opened = new Promise((res, rej) => {
    const t = setTimeout(() => rej(new Error('等 WebSocket open 超时（10s）')), 10_000);
    openResolve = () => { clearTimeout(t); res(); };
  });
  const stream = api.events.mux({}, undefined, () => openResolve());

  const done = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('等她回复超时（120s）')), 120_000);
    (async () => {
      for await (const envelope of stream) {
        const frame = envelope.payload;
        if (frame.type === 'session/event' && frame.sessionId === sessionId) {
          if (streamMode && frame.event?.type === 'assistant/chunk') {
            const piece = chunkText(frame.event.data);
            if (piece) {
              if (!sawChunk) { sawChunk = true; log('PROBE first chunk: ' + JSON.stringify(frame.event.data).slice(0, 400)); }
              process.stdout.write('S:' + JSON.stringify(piece) + '\n');
            }
          }
          const ended = collector.push(frame.event);
          if (ended) { clearTimeout(timer); resolve(ended); return; }
        }
        if (frame.type === 'stream/error') {
          clearTimeout(timer);
          reject(new Error('事件流错误: ' + JSON.stringify(frame.error)));
          return;
        }
      }
    })().catch((e) => { clearTimeout(timer); reject(e); });
  });

  await opened;
  stream.follow(sessionId);

  log(`🗣  你 → 她：「${text}」`);
  unwrap(await api.sessions.prompt({
    sessionId, mode: 'queue', content: [{ type: 'text', text: payload }],
  }), 'session.prompt');

  const ended = await done;
  const reply = String(ended.text ?? '').trim();
  log(`💭 回合结束 reason=${ended.reason?.kind ?? '?'}，${reply.length} 字`);
  if (streamMode) process.stdout.write('F:' + JSON.stringify(reply) + '\n');
  else process.stdout.write(reply + '\n');
  process.exit(0);
}

main().catch((e) => {
  log('❌ ' + (e?.message ?? e));
  process.exit(1);
});
