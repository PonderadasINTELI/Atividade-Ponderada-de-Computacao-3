// Ponte MQTT -> navegador. Uso: npm install && npm start, depois http://localhost:3939
// npm run rede: abre tambem no Wi-Fi, para ver no celular, so se o notebook estiver na rede do ESP32.
const fs = require('fs'), http = require('http'), os = require('os'), path = require('path'), mqtt = require('mqtt');

const src = fs.readFileSync(process.env.SECRETS || path.join(__dirname, '..', 'rssi-wifi', 'secrets.h'), 'utf8');
const cfg = {};
for (const m of src.matchAll(/^[ \t]*#define[ \t]+(\w+)[ \t]+(?:"([^"]*)"|(\d+))/gm)) cfg[m[1]] = m[2] ?? Number(m[3]);
if (!cfg.MQTT_HOST || cfg.MQTT_HOST === 'PREENCHER') { console.error('Preencha MQTT_HOST em secrets.h'); process.exit(1); }

// Mesma ordem de MODOS em rssi-wifi.ino: o ESP32 envia só o número.
const CONDICOES = ['', 'Fora do metal (antes)', 'Dentro do metal', 'Fora do metal (depois)', 'Descoberta',
  'Coberta com as mãos', 'Vertical', 'Horizontal', 'Perto do roteador', 'Longe do roteador'];
const PORTA = Number(process.env.PORT) || 3939;
const AMOSTRAS = 5; // por condição, igual ao index.html; as seguintes não contam
const SEM_DADOS_MS = 7000; // 3 leituras de 2 s perdidas = interrupção
let csv;
function novoCsv() {
  csv = path.join(__dirname, `amostras-${new Date().toISOString().replace(/[:.]/g, '-')}.csv`);
  fs.writeFileSync(csv, 'hora;rssi_dbm;seq;condicao\n');
}
novoCsv();

const eventos = [], clientes = new Set();
let modoPlaca, pedido = null, zerando = false, condicao = '', ultima = 0, interrompido = false, broker = false;
const enviar = e => clientes.forEach(r => r.write(`data: ${JSON.stringify(e)}\n\n`));
const registrar = e => { eventos.push(e); enviar(e); };
const estado = () => ({ tipo: 'estado', condicao, broker, condicoes: CONDICOES });

const cli = mqtt.connect({
  protocol: cfg.MQTT_TLS ? 'mqtts' : 'mqtt', host: cfg.MQTT_HOST, port: cfg.MQTT_PORT,
  username: cfg.MQTT_USER || undefined, password: cfg.MQTT_PASS || undefined,
  clientId: 'rssi-painel-' + Math.random().toString(16).slice(2, 8), reconnectPeriod: 3000,
});
cli.on('connect', () => cli.subscribe(cfg.MQTT_TOPIC, (err, g) => {
  const recusado = err || g.some(x => x.qos > 2);
  console.log(recusado ? `[mqtt] assinatura de ${cfg.MQTT_TOPIC} recusada` : `[mqtt] assinando ${cfg.MQTT_TOPIC}`);
  broker = !recusado; enviar(estado());
}));
// Sem conexão não enfileira: um pedido velho chegaria depois de um toque na placa.
function pedirModo() { if (cli.connected) cli.publish(`${cfg.MQTT_TOPIC}/cmd`, `{"modo":${pedido}}`); }
// Zera o gráfico e as médias e manda a placa para o primeiro passo. O CSV anterior fica guardado.
function zerar() {
  eventos.length = 0; ultima = 0; interrompido = false; condicao = ''; pedido = 0; zerando = true;
  novoCsv();
  if (cli.connected) cli.publish(`${cfg.MQTT_TOPIC}/cmd`, '{"reset":1}');
  enviar({ tipo: 'inicio', eventos, agora: Date.now() }); enviar(estado());
}
cli.on('close', () => { if (broker) { broker = false; enviar(estado()); } });
cli.on('error', e => console.error('[mqtt]', e.message));
cli.on('message', (_t, buf) => {
  let d; try { d = JSON.parse(buf); } catch { return; }
  if (!d || !Number.isInteger(d.rssi) || d.rssi >= 0 || d.rssi < -127) return;
  const t = Date.now();
  if (interrompido) { interrompido = false; registrar({ tipo: 'retomada', t }); }
  ultima = t;
  // Vale a última ação. Um clique na página vira pedido, reenviado até a placa confirmar; um toque
  // na placa muda o modo dela e cancela o pedido.
  let medida = condicao; // sketch sem "modo": vale o que estiver marcado na página
  if (Number.isInteger(d.modo) && CONDICOES[d.modo] !== undefined) {
    medida = CONDICOES[d.modo]; // modo em que a placa mediu, mesmo que a amostra chegue depois de um clique
    if (modoPlaca === undefined && pedido !== null) modoPlaca = d.modo; // clique antes da primeira amostra: não é toque na placa
    if (d.modo === 0 || d.modo !== modoPlaca) zerando = false;
    if (zerando) medida = ''; // amostra de antes do reset, ainda a caminho
    if (d.modo !== modoPlaca) {
      modoPlaca = d.modo; pedido = null; condicao = CONDICOES[d.modo]; enviar(estado());
    } else if (d.modo === pedido) pedido = null;
    else if (pedido !== null) pedirModo();
  }
  if (eventos.filter(e => e.condicao === medida).length >= AMOSTRAS) medida = '';
  registrar({ tipo: 'amostra', t, rssi: d.rssi, seq: Number(d.seq) || 0, condicao: medida });
  fs.appendFileSync(csv, `${new Date(t).toISOString()};${d.rssi};${Number(d.seq) || 0};${medida}\n`);
});
setInterval(() => {
  if (ultima && !interrompido && Date.now() - ultima > SEM_DADOS_MS) {
    interrompido = true;
    registrar({ tipo: 'interrupcao', t: ultima, painel: !broker }); // painel: quem caiu foi o dashboard
  }
}, 1000);

const arquivos = {
  '/': [path.join(__dirname, 'index.html'), 'text/html; charset=utf-8'],
  '/chart.js': [path.join(__dirname, 'node_modules/chart.js/dist/chart.umd.js'), 'text/javascript'],
};
for (const f of ['fredoka-600', 'atkinson-hyperlegible-400', 'atkinson-hyperlegible-700', 'jetbrains-mono-400'])
  arquivos[`/assets/${f}.ttf`] = [path.join(__dirname, `assets/${f}.ttf`), 'font/ttf'];
const atender = (req, res) => {
  if (req.url === '/eventos') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
    res.write(`data: ${JSON.stringify({ tipo: 'inicio', eventos, agora: Date.now() })}\n\n`);
    res.write(`data: ${JSON.stringify(estado())}\n\n`);
    clientes.add(res); req.on('close', () => clientes.delete(res)); return;
  }
  // So a propria pagina troca o modo: outro site aberto no navegador nao consegue.
  const origem = req.headers.origin;
  if (origem && origem !== `http://${req.headers.host}`) return res.writeHead(403).end();
  if (req.method === 'POST' && req.url === '/zerar') { zerar(); return res.writeHead(204).end(); }
  const m = req.method === 'POST' && req.url.match(/^\/condicao\/(\d+)$/);
  if (m && CONDICOES[m[1]] !== undefined) {
    pedido = Number(m[1]); condicao = CONDICOES[pedido]; pedirModo(); enviar(estado());
    return res.writeHead(204).end();
  }
  const a = arquivos[req.url];
  if (!a) return res.writeHead(404).end();
  res.writeHead(200, { 'Content-Type': a[1] }).end(fs.readFileSync(a[0]));
};
const abrir = (ip, nome) => http.createServer(atender).on('error', e => { console.error(e.message); process.exit(1); })
  .listen(PORTA, ip, () => console.log(`${nome}: http://${ip === '127.0.0.1' ? 'localhost' : ip}:${PORTA}`));
abrir('127.0.0.1', 'Dashboard');

if (process.argv.includes('--rede')) {
  const ascii = s => s.replace(/[^ -~]/g, '').trim(); // o netsh embaralha acentos
  let ssid = '';
  try { ssid = require('child_process').execSync('netsh wlan show interfaces', { encoding: 'latin1' }).match(/^\s*SSID\s*:\s*(.*)$/m)?.[1] ?? ''; } catch {}
  const ip = (os.networkInterfaces()['Wi-Fi'] || []).find(i => i.family === 'IPv4')?.address;
  if (ip && ssid && ascii(ssid) === ascii(cfg.WIFI_SSID || '')) abrir(ip, 'No celular');
  else console.log('Celular: conecte o notebook no mesmo Wi-Fi do ESP32 e rode de novo. Por enquanto, so neste notebook.');
}
