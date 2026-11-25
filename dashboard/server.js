// Ponte MQTT -> navegador. Uso: npm install && npm start, depois http://localhost:3939
const fs = require('fs'), http = require('http'), path = require('path'), mqtt = require('mqtt');

const src = fs.readFileSync(process.env.SECRETS || path.join(__dirname, '..', 'rssi-wifi', 'secrets.h'), 'utf8');
const cfg = {};
for (const m of src.matchAll(/^[ \t]*#define[ \t]+(\w+)[ \t]+(?:"([^"]*)"|(\d+))/gm)) cfg[m[1]] = m[2] ?? Number(m[3]);
if (!cfg.MQTT_HOST || cfg.MQTT_HOST === 'PREENCHER') { console.error('Preencha MQTT_HOST em secrets.h'); process.exit(1); }

const CONDICOES = ['', 'Fora do metal (antes)', 'Dentro do metal', 'Fora do metal (depois)', 'Descoberta',
  'Coberta com as mãos', 'Vertical', 'Horizontal', 'Perto do roteador', 'Longe do roteador'];
const PORTA = Number(process.env.PORT) || 3939;
const SEM_DADOS_MS = 7000; // 3 leituras de 2 s perdidas = interrupção
const csv = path.join(__dirname, `amostras-${new Date().toISOString().replace(/[:.]/g, '-')}.csv`);
fs.writeFileSync(csv, 'hora;rssi_dbm;seq;condicao\n');

const eventos = [], clientes = new Set();
let condicao = '', ultima = 0, interrompido = false, broker = false;
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
cli.on('close', () => { if (broker) { broker = false; enviar(estado()); } });
cli.on('error', e => console.error('[mqtt]', e.message));
cli.on('message', (_t, buf) => {
  let d; try { d = JSON.parse(buf); } catch { return; }
  if (!d || !Number.isInteger(d.rssi) || d.rssi >= 0 || d.rssi < -127) return;
  const t = Date.now();
  if (interrompido) { interrompido = false; registrar({ tipo: 'retomada', t }); }
  ultima = t;
  registrar({ tipo: 'amostra', t, rssi: d.rssi, seq: Number(d.seq) || 0, condicao });
  fs.appendFileSync(csv, `${new Date(t).toISOString()};${d.rssi};${Number(d.seq) || 0};${condicao}\n`);
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
http.createServer((req, res) => {
  if (req.url === '/eventos') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
    res.write(`data: ${JSON.stringify({ tipo: 'inicio', eventos })}\n\n`);
    res.write(`data: ${JSON.stringify(estado())}\n\n`);
    clientes.add(res); req.on('close', () => clientes.delete(res)); return;
  }
  const m = req.method === 'POST' && req.url.match(/^\/condicao\/(\d+)$/);
  if (m && CONDICOES[m[1]] !== undefined) { condicao = CONDICOES[m[1]]; enviar(estado()); return res.writeHead(204).end(); }
  const a = arquivos[req.url];
  if (!a) return res.writeHead(404).end();
  res.writeHead(200, { 'Content-Type': a[1] }).end(fs.readFileSync(a[0]));
}).listen(PORTA, '127.0.0.1', () => console.log(`Dashboard: http://localhost:${PORTA}`));
