const fs = require('fs');
const D = require('docx');
const { Document, Packer, Paragraph, TextRun, Table, TableRow, TableCell, WidthType,
  AlignmentType, BorderStyle, ShadingType, LevelFormat, VerticalAlign, TableLayoutType, HeightRule } = D;

const FONT = 'Arial', SZ = 19, MONO = 'Courier New';
const PAGE_W = 11906, MARGIN = 640, CW = PAGE_W - 2 * MARGIN;

// texto com **negrito** e `código`
function runs(txt, extra = {}) {
  const out = [];
  txt.split(/(\*\*[^*]+\*\*|`[^`]+`)/).forEach(p => {
    if (!p) return;
    if (p.startsWith('**')) out.push(new TextRun({ text: p.slice(2, -2), bold: true, font: FONT, size: SZ, ...extra }));
    else if (p.startsWith('`')) out.push(new TextRun({ text: p.slice(1, -1), font: MONO, size: SZ - 1, ...extra }));
    else out.push(new TextRun({ text: p, font: FONT, size: SZ, ...extra }));
  });
  return out;
}
const P = (t, o = {}) => new Paragraph({ children: runs(t, o.run), alignment: AlignmentType.JUSTIFIED,
  spacing: { before: 0, after: 50, line: 240 }, ...o.par });
const H = t => new Paragraph({ children: [new TextRun({ text: t, bold: true, font: FONT, size: 20, color: '1F3864' })],
  spacing: { before: 90, after: 40 }, keepNext: true,
  border: { bottom: { style: BorderStyle.SINGLE, size: 4, color: '1F3864', space: 1 } } });
const B = t => new Paragraph({ children: runs(t), numbering: { reference: 'bul', level: 0 },
  alignment: AlignmentType.JUSTIFIED, spacing: { before: 0, after: 30, line: 240 } });
const CODE = lines => lines.map((l, i) => new Paragraph({
  children: [new TextRun({ text: l, font: MONO, size: 14 })],
  shading: { type: ShadingType.CLEAR, fill: 'F2F2F2', color: 'auto' },
  spacing: { before: 0, after: i === lines.length - 1 ? 60 : 0, line: 190 } }));

const thin = { style: BorderStyle.SINGLE, size: 2, color: 'A6A6A6' };
const borders = { top: thin, bottom: thin, left: thin, right: thin };
const none = { style: BorderStyle.NONE, size: 0, color: 'FFFFFF' };
const noBorders = { top: none, bottom: none, left: none, right: none };

// ---------- tabela de resultados ----------
const cols = [2100, 2550, 3350, 2600];
const hdr = ['Política (CPU s)', 'Despachos A/B/C/D', 'Caracteres A/B/C/D', 'CPU % A/B/C/D'];
const rows = [
  ['SCHED_FIFO (1,0009)', '2 / 1 / 1 / 1', '5.000.000 / 0 / 0 / 0', '99,95 / 0,00 / 0,01 / 0,04'],
  ['SCHED_RR (1,9094)', '10 / 9 / 9 / 8', '1.206.817 / 1.777.829 / 1.234.617 / 780.737', '20,98 / 31,39 / 26,47 / 21,17'],
  ['SCHED_IDLE (1,9726)', '211 / 213 / 213 / 210', '1.254.514 / 1.258.704 / 1.169.865 / 1.316.917', '26,01 / 24,67 / 23,12 / 26,20'],
  ['SCHED_LOW_IDLE (2,4064)', '250 / 241 / 237 / 240', '1.250.070 / 1.309.042 / 1.288.497 / 1.152.391', '24,63 / 26,24 / 25,59 / 23,54'],
  ['Misto: LOW_IDLE/IDLE (1,1545)', '182 / 184', '1.700.693 / 3.299.307', '29,88 / 70,12'],
];
const cell = (t, w, head, i) => new TableCell({
  width: { size: w, type: WidthType.DXA }, borders, verticalAlign: VerticalAlign.CENTER,
  shading: { type: ShadingType.CLEAR, fill: head ? 'D9E2F3' : 'FFFFFF', color: 'auto' },
  margins: { top: 20, bottom: 20, left: 60, right: 60 },
  children: [new Paragraph({ alignment: i === 0 ? AlignmentType.LEFT : AlignmentType.CENTER, spacing: { before: 0, after: 0 },
    children: [new TextRun({ text: t, font: FONT, size: 16, bold: head })] })] });
const tabela = new Table({ width: { size: CW, type: WidthType.DXA }, columnWidths: cols, layout: TableLayoutType.FIXED,
  rows: [new TableRow({ tableHeader: true, children: hdr.map((t, i) => cell(t, cols[i], true, i)) }),
    ...rows.map(r => new TableRow({ children: r.map((t, i) => cell(t, cols[i], false, i)) }))] });

// ---------- espaços para os prints do KernelShark ----------
const placeholder = label => new TableCell({
  width: { size: CW / 2, type: WidthType.DXA }, borders,
  verticalAlign: VerticalAlign.CENTER,
  margins: { top: 100, bottom: 100, left: 80, right: 80 },
  children: [new Paragraph({
    alignment: AlignmentType.CENTER,
    spacing: { before: 0, after: 0 },
    children: [new TextRun({
      text: `INSERIR PRINT: ${label || 'área livre'}`,
      font: FONT,
      size: 17,
      color: '666666'
    })]
  })]
});
const prints = new Table({
  width: { size: CW, type: WidthType.DXA },
  columnWidths: [CW / 2, CW / 2],
  layout: TableLayoutType.FIXED,
  rows: [
    new TableRow({ cantSplit: true, height: { value: 1800, rule: HeightRule.ATLEAST }, children: [placeholder('SCHED_FIFO'), placeholder('SCHED_RR')] }),
    new TableRow({ cantSplit: true, height: { value: 1800, rule: HeightRule.ATLEAST }, children: [placeholder('SCHED_IDLE'), placeholder('SCHED_LOW_IDLE')] }),
    new TableRow({ cantSplit: true, height: { value: 1800, rule: HeightRule.ATLEAST }, children: [placeholder('MISTO: LOW_IDLE + IDLE'), placeholder('')] })
  ]
});

// ---------- documento ----------
const doc = new Document({
  styles: { default: { document: { run: { font: FONT, size: SZ } } } },
  numbering: { config: [{ reference: 'bul', levels: [{ level: 0, format: LevelFormat.BULLET, text: '•', alignment: AlignmentType.LEFT,
    style: { paragraph: { indent: { left: 240, hanging: 180 } } } }] }] },
  sections: [{
    properties: { page: { size: { width: PAGE_W, height: 16838 }, margin: { top: 600, bottom: 560, left: MARGIN, right: MARGIN } } },
    children: [
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 0, after: 20 },
        children: [new TextRun({ text: 'TP3 — CPU Scheduling Profiler', bold: true, font: FONT, size: 28, color: '1F3864' })] }),
      new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 0, after: 60 },
        children: [new TextRun({ text: 'Laboratório de Sistemas Operacionais — PUCRS 2026/2  ·  Pedro Jardim Machado e Lucas Lorenzi da Silva  ·  10/11/2026', font: FONT, size: 16, color: '404040' })] }),

      H('1. Objetivo e ambiente'),
      P('Avaliar FIFO, RR, IDLE e LOW_IDLE no Linux 4.13.9 (Buildroot/i386) em QEMU. Os testes principais usaram 1 CPU, buffer de 5.000.000 posições, quatro threads (duas no misto: A=LOW_IDLE, B=IDLE) e traces `sched_switch` analisados no KernelShark. O kernel usa `WEIGHT_LOW_IDLEPRIO=1` e `WMULT_LOW_IDLEPRIO=4294967295`.'),

      H('2. Implementação e medida'),
      P('Cada thread grava sua letra na próxima posição do buffer; um semáforo binário protege o índice. As políticas são aplicadas antes da largada; FIFO/RR usam prioridade 10. O programa compacta letras consecutivas e imprime escalonamentos estimados (trocas de contexto + primeira entrada), caracteres escritos e CPU por thread.'),
      ...CODE([
        '$ ./sched_profiler 5000000 4 SCHED_RR',
        'Pos-processado: ABCDABCDABCDABCD',
        'A: 10 escalonamentos, 1.206.817 caracteres, 20,98% CPU',
        'B:  9 escalonamentos, 1.777.829 caracteres, 31,39% CPU',
        'C:  9 escalonamentos, 1.234.617 caracteres, 26,47% CPU',
        'D:  8 escalonamentos,   780.737 caracteres, 21,17% CPU',
      ]),

      H('3. Resultados (uma execução por caso)'),
      P('A ordem nas células é A/B/C/D; no teste misto, A/B. O tempo entre parênteses é CPU total das threads, em segundos.'),
      tabela,
      new Paragraph({ spacing: { before: 20, after: 30 }, children: runs('**Tabela 1.** CPU é o tempo somado das threads; percentuais são relativos a essa soma. Os despachos do programa são estimados; confira-os nos traces.', { size: 14 }) }),
      P('Comparação suplementar com 2 CPUs (quatro threads): FIFO, 10,642 s de CPU total e 25,47/26,74/24,38/23,41%; RR, 10,271 s e 24,20/24,69/26,18/24,93%. O kernel registrou `RT throttling activated` no FIFO.'),

      new Paragraph({
        children: [new TextRun({ text: '4. Análise e justificativa', bold: true, font: FONT, size: 20, color: '1F3864' })],
        spacing: { before: 90, after: 40 },
        keepNext: true,
        pageBreakBefore: true,
        border: { bottom: { style: BorderStyle.SINGLE, size: 4, color: '1F3864', space: 1 } }
      }),
      P('**SCHED_FIFO.** A recebeu 99,95% do tempo de CPU e escreveu o buffer inteiro; B, C e D não escreveram. Com a mesma prioridade, A não bloqueia no laço e segue executando até terminar. O resultado ilustra o FIFO entre iguais e o risco de uma tarefa RT monopolizar a CPU.'),
      P('**SCHED_RR.** As quatro threads usam prioridade 10; ao expirar a fatia de 100 ms, uma thread pronta cede lugar à próxima. Esse timeout é o fim do quantum, não um erro ou uma espera. Nesta rodada, houve 10/9/9/8 despachos estimados, mas a CPU variou de 20,98% a 31,39%. Despachos contam entradas, não duração; a contenção e os bloqueios no semáforo afetam os tempos. O trace permite conferir o rodízio real.'),
      P('**SCHED_IDLE e SCHED_LOW_IDLE.** Em cada execução homogênea, todas as threads têm o mesmo peso e dividiram a CPU aproximadamente por igual. Para o misto, os pesos 3:1 (IDLE:LOW_IDLE) sugerem 75/25; medimos 70,12/29,88. A diferença é plausível pela contenção no semáforo e pelo término ao preencher o buffer. Ambas são políticas de trabalho de fundo; LOW_IDLE tem peso menor.'),
      P('**Projeto, usos e limites.** O semáforo evita escritas concorrentes, mas serializa a seção crítica. FIFO pode atender uma tarefa RT que não deve ser interrompida por iguais; RR reparte a CPU entre tarefas RT da mesma prioridade. IDLE/LOW_IDLE servem a manutenção em segundo plano e cedem CPU às classes superiores. Deadline não foi avaliada. Os dados são uma rodada por caso, sem médias.'),

      new Paragraph({
        children: [new TextRun({ text: '5. Traces no KernelShark — inserir os prints', bold: true, font: FONT, size: 20, color: '1F3864' })],
        spacing: { before: 90, after: 40 },
        keepNext: true,
        border: { bottom: { style: BorderStyle.SINGLE, size: 4, color: '1F3864', space: 1 } }
      }),
      new Paragraph({ spacing: { before: 0, after: 35 }, children: runs('Insira um print de cada trace, identificando a política e as threads.', { size: 15 }) }),
      prints,
    ] }] });

Packer.toBuffer(doc).then(b => {
  fs.writeFileSync('/workspaces/labsisop-buildroot/relatorio/TP3_relatorio.docx', b);
  fs.writeFileSync('/workspaces/labsisop-buildroot/relatorio/relatorio/TP3_relatorio.docx', b);
  console.log('ok');
});
