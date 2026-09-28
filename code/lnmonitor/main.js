// ═══════════════════════════════════════════════════════════════════════════════════════════════
// DATA SOURCE CONFIGURATION
// ═══════════════════════════════════════════════════════════════════════════════════════════════
const DATA_BASE_URL = 'data';

const DATA_PATHS = {
  dailyMetrics: `${DATA_BASE_URL}/daily_metrics.json`,
  dictionary: `${DATA_BASE_URL}/channels.csv`,
  channels: (channelId) => `${DATA_BASE_URL}/edges/${channelId}.csv`,
  channelMetadata: (channelId) => `${DATA_BASE_URL}/edges_meta_data/${channelId}.csv`,
};

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// THEME SWITCHER
// ═══════════════════════════════════════════════════════════════════════════════════════════════

const themeToggle = document.getElementById('theme-toggle');
const isDarkPreferred = window.matchMedia('(prefers-color-scheme: dark)').matches;
const savedTheme = localStorage.getItem('theme');
const initialTheme = savedTheme || (isDarkPreferred ? 'dark' : 'light');

if (initialTheme === 'light') {
  document.body.classList.add('light-mode');
  themeToggle.textContent = '☀️';
}

themeToggle.addEventListener('click', () => {
  const isLightMode = document.body.classList.toggle('light-mode');
  themeToggle.textContent = isLightMode ? '☀️' : '🌙';
  localStorage.setItem('theme', isLightMode ? 'light' : 'dark');
  // Re-render charts with new colors
  if (networkData) {
    renderCharts();
  }
});

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// NAV TAB SWITCHING
// ═══════════════════════════════════════════════════════════════════════════════════════════════

const navTabs = document.querySelectorAll('.nav-tab');
const networkView = document.getElementById('network-view');
const channelView = document.getElementById('channel-view');

navTabs.forEach(tab => {
  tab.addEventListener('click', () => {
    const view = tab.dataset.view;
    navTabs.forEach(t => t.classList.remove('active'));
    tab.classList.add('active');

    // Hide all views
    document.getElementById('network-view').classList.add('hidden');
    document.getElementById('channel-view').classList.add('hidden');
    document.getElementById('node-view').classList.add('hidden');
    document.getElementById('download-view').classList.add('hidden');

    if (view === 'network-view') {
      document.getElementById('network-view').classList.remove('hidden');
      loadNetworkMetrics();
    } else if (view === 'channel-view') {
      document.getElementById('channel-view').classList.remove('hidden');
    } else if (view === 'node-view') {
      document.getElementById('node-view').classList.remove('hidden');
    } else if (view === 'download-view') {
      document.getElementById('download-view').classList.remove('hidden');
      loadDownloadData();
    }
  });
});

// Load network metrics on page load (default view is network overview)
window.addEventListener('load', loadNetworkMetrics);

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// NETWORK METRICS
// ═══════════════════════════════════════════════════════════════════════════════════════════════

let networkData = null;
let charts = {};

async function loadNetworkMetrics() {
  if (networkData) return;

  try {
    const res = await fetch(DATA_PATHS.dailyMetrics);
    if (!res.ok) return;
    networkData = await res.json();
    renderNetworkSummary();
    renderCharts();
  } catch (e) {
    console.error('Failed to load network metrics:', e);
  }
}

function renderNetworkSummary() {
  const latest = networkData[networkData.length - 1];
  if (!latest) return;

  const capacityBTC = (latest.total_capacity / 100000000).toFixed(0);

  const cards = [
    { value: latest.num_nodes.toLocaleString(), label: 'Total Nodes' },
    { value: latest.num_edges.toLocaleString(), label: 'Channels' },
    { value: capacityBTC, label: 'Capacity (BTC)' },
    { value: latest.avg_degree.toFixed(2), label: 'Avg Degree' },
    { value: (latest.avg_clustering * 100).toFixed(2) + '%', label: 'Clustering Coeff' },
    { value: latest.num_components.toLocaleString(), label: 'Components' },
  ];

  const container = document.getElementById('network-summary-cards');
  container.innerHTML = cards
    .map(
      card => `
    <div class="stat-card">
      <div class="value">${card.value}</div>
      <div class="label">${card.label}</div>
    </div>
  `
    )
    .join('');
}

function renderCharts() {
  const dates = networkData.map(d => d.date);
  const colors = { accent: '#f5a623', accent2: '#66bb6a', accent3: '#ec407a', grid: '#1e1e2e', text: '#666' };

  // Chart 1: Network Size (Nodes + Edges, dual axis)
  renderChart(
    'chart-nodes-edges',
    'Network Size',
    {
      labels: dates,
      datasets: [
        {
          label: 'Nodes',
          data: networkData.map(d => d.num_nodes),
          borderColor: colors.accent,
          backgroundColor: 'rgba(245, 166, 35, 0.1)',
          borderWidth: 1.5,
          tension: 0.3,
          pointRadius: 0,
          yAxisID: 'y',
        },
        {
          label: 'Channels (Edges)',
          data: networkData.map(d => d.num_edges),
          borderColor: colors.accent2,
          backgroundColor: 'rgba(102, 187, 106, 0.1)',
          borderWidth: 1.5,
          tension: 0.3,
          pointRadius: 0,
          yAxisID: 'y1',
        },
      ],
    },
    {
      y: { position: 'left', title: { text: 'Nodes' } },
      y1: { position: 'right', title: { text: 'Channels' } },
    }
  );

  // Chart 2: Total Capacity
  renderChart(
    'chart-capacity',
    'Total Capacity',
    {
      labels: dates,
      datasets: [
        {
          label: 'Capacity (BTC)',
          data: networkData.map(d => (d.total_capacity / 100000000).toFixed(0)),
          borderColor: colors.accent,
          backgroundColor: 'rgba(245, 166, 35, 0.1)',
          borderWidth: 1.5,
          tension: 0.3,
          pointRadius: 0,
          fill: true,
        },
      ],
    }
  );

  // Chart 3: Connectivity
  renderChart(
    'chart-connectivity',
    'Connectivity Metrics',
    {
      labels: dates,
      datasets: [
        {
          label: 'Avg Degree',
          data: networkData.map(d => d.avg_degree),
          borderColor: colors.accent,
          backgroundColor: 'rgba(245, 166, 35, 0.1)',
          borderWidth: 1.5,
          tension: 0.3,
          pointRadius: 0,
          yAxisID: 'y',
        },
        {
          label: 'Avg Clustering',
          data: networkData.map(d => d.avg_clustering),
          borderColor: colors.accent2,
          backgroundColor: 'rgba(102, 187, 106, 0.1)',
          borderWidth: 1.5,
          tension: 0.3,
          pointRadius: 0,
          yAxisID: 'y1',
        },
      ],
    },
    {
      y: { position: 'left', title: { text: 'Avg Degree' } },
      y1: { position: 'right', title: { text: 'Clustering' } },
    }
  );

  // Chart 4: Topology
  renderChart(
    'chart-topology',
    'Topology Structure',
    {
      labels: dates,
      datasets: [
        {
          label: 'Components',
          data: networkData.map(d => d.num_components),
          borderColor: colors.accent,
          backgroundColor: 'rgba(245, 166, 35, 0.1)',
          borderWidth: 1.5,
          tension: 0.3,
          pointRadius: 0,
          yAxisID: 'y',
        },
        {
          label: 'Largest Component Size',
          data: networkData.map(d => d.largest_component_size),
          borderColor: colors.accent3,
          backgroundColor: 'rgba(236, 64, 122, 0.1)',
          borderWidth: 1.5,
          tension: 0.3,
          pointRadius: 0,
          yAxisID: 'y1',
        },
      ],
    },
    {
      y: { position: 'left', title: { text: 'Components' } },
      y1: { position: 'right', title: { text: 'Largest Component' } },
    }
  );
}

function renderChart(elementId, title, data, axes = {}) {
  const ctx = document.getElementById(elementId).getContext('2d');

  if (charts[elementId]) {
    charts[elementId].destroy();
  }

  const isLightMode = document.body.classList.contains('light-mode');
  const gridColor = isLightMode ? '#e8e8e8' : '#1a1a24';
  const textColor = isLightMode ? '#555' : '#666';
  const tooltipBg = isLightMode ? '#ffffff' : '#1a1a24';
  const tooltipBorder = isLightMode ? '#e0e0e0' : '#1e1e2e';
  const tooltipText = isLightMode ? '#333' : '#ccc';

  const scales = {
    x: {
      title: { display: false },
      grid: { color: gridColor, drawBorder: false },
      ticks: { color: textColor, font: { size: 11 } },
    },
    y: {
      position: 'left',
      title: { display: true, text: axes.y?.title?.text, color: textColor },
      grid: { color: gridColor },
      ticks: { color: textColor, font: { size: 11 } },
    },
  };

  if (axes.y1) {
    scales.y1 = {
      position: 'right',
      title: { display: true, text: axes.y1?.title?.text, color: textColor },
      grid: { drawOnChartArea: false },
      ticks: { color: textColor, font: { size: 11 } },
    };
  }

  charts[elementId] = new Chart(ctx, {
    type: 'line',
    data: data,
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: {
          labels: { color: textColor, font: { size: 12, weight: '500' } },
          padding: 15,
        },
        tooltip: {
          backgroundColor: tooltipBg,
          titleColor: '#f5a623',
          bodyColor: tooltipText,
          borderColor: tooltipBorder,
          borderWidth: 1,
          padding: 10,
          displayColors: true,
          font: { size: 11 },
        },
      },
      scales: scales,
    },
  });
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// CHANNEL EXPLORER
// ═══════════════════════════════════════════════════════════════════════════════════════════════

const form = document.getElementById('search-form');
const input = document.getElementById('channel-input');
const results = document.getElementById('results');
const errorMsg = document.getElementById('error-msg');
const tableBody = document.getElementById('updates-table-body');

let dictionary = null;

async function loadDictionary() {
  if (dictionary) return;
  try {
    const res = await fetch(DATA_PATHS.dictionary);
    if (!res.ok) return;
    const text = await res.text();
    const lines = text.trim().split('\n');
    const headers = lines[0].split(',').map(h => h.trim());
    dictionary = {};
    for (let i = 1; i < lines.length; i++) {
      const values = lines[i].split(',');
      const row = {};
      headers.forEach((h, idx) => {
        row[h] = values[idx]?.trim() ?? '';
      });
      dictionary[row.channel_id] = row;
    }
  } catch {
    // dictionary unavailable, continue without it
  }
}

form.addEventListener('submit', async e => {
  e.preventDefault();
  const channelId = input.value.trim();
  if (!channelId) return;
  await loadChannel(channelId);
});

async function loadChannel(channelId) {
  errorMsg.classList.add('hidden');
  results.classList.add('hidden');
  tableBody.innerHTML = '';

  const url = DATA_PATHS.channels(channelId);
  const metaUrl = DATA_PATHS.channelMetadata(channelId);

  let text, metaText;
  try {
    const [channelRes, metaRes] = await Promise.all([
      fetch(url),
      fetch(metaUrl),
    ]);
    if (!channelRes.ok) throw new Error('not found');
    text = await channelRes.text();
    metaText = metaRes.ok ? await metaRes.text() : null;
  } catch {
    errorMsg.textContent = `Channel "${channelId}" not found.`;
    errorMsg.classList.remove('hidden');
    return;
  }

  const rows = parseCSV(text);
  if (rows.length === 0) {
    errorMsg.textContent = 'CSV is empty or malformed.';
    errorMsg.classList.remove('hidden');
    return;
  }

  const metaRows = metaText ? parseCSV(metaText) : [];

  window.currentChannelRows = rows; // Store for chart access
  renderSummary(channelId, rows, metaRows);
  renderChannelCharts(channelId, rows);
  renderTable(rows);
  results.classList.remove('hidden');
}

function parseCSV(text) {
  const lines = text.trim().split('\n');
  if (lines.length < 2) return [];

  const headers = lines[0].split(',').map(h => h.trim());
  const rows = [];

  for (let i = 1; i < lines.length; i++) {
    const values = lines[i].split(',');
    const row = {};
    headers.forEach((h, idx) => {
      row[h] = values[idx] !== undefined ? values[idx].trim() : '';
    });
    rows.push(row);
  }

  return rows;
}

function renderSummary(channelId, rows, metaRows) {
  document.getElementById('channel-id-label').textContent = channelId;

  const sorted = [...rows].sort((a, b) => parseInt(a.recording_time) - parseInt(b.recording_time));
  const disabledCount = rows.filter(r => r.disabled === '1' || r.disabled === 'True').length;

  const meta = dictionary?.[channelId];
  const edgeMeta = metaRows.length > 0 ? metaRows[0] : null;

  // Determine node order by comparing pubkeys (smallest = 1)
  const node1Pub = edgeMeta?.node1Pub || meta?.node_1 || '';
  const node2Pub = edgeMeta?.node2Pub || meta?.node_2 || '';
  const isNode1Smaller = node1Pub < node2Pub;

  // Map advertiser to correct node based on pubkey order
  let sender1Rows, sender2Rows;
  if (isNode1Smaller) {
    sender1Rows = sorted.filter(r => r.advertiser === '1');
    sender2Rows = sorted.filter(r => r.advertiser === '2');
  } else {
    sender1Rows = sorted.filter(r => r.advertiser === '2');
    sender2Rows = sorted.filter(r => r.advertiser === '1');
  }

  const latestSender1 = sender1Rows[sender1Rows.length - 1];
  const latestSender2 = sender2Rows[sender2Rows.length - 1];

  // Overview section
  document.getElementById('stat-updates').textContent = rows.length.toLocaleString();
  document.getElementById('stat-disabled').textContent = disabledCount.toLocaleString();

  // Node 1
  if (latestSender1) {
    const state1 = latestSender1.disabled === '1' || latestSender1.disabled === 'True' ? 'Disabled' : 'Active';
    const date1 = latestSender1.recording_time && !isNaN(parseInt(latestSender1.recording_time))
      ? new Date(parseInt(latestSender1.recording_time) * 1000).toISOString().slice(0, 10)
      : '—';

    document.getElementById('stat-node1-state').textContent = state1;
    document.getElementById('stat-node1-timestamp').textContent = latestSender1.recording_time ?? '—';
    document.getElementById('stat-node1-date').textContent = date1;
    document.getElementById('stat-node1-fee-base').textContent = latestSender1.feeBaseMsat ?? '—';
    document.getElementById('stat-node1-fee-rate').textContent = latestSender1.feeRateMilliMsat ?? '—';
    document.getElementById('stat-node1-min-htlc').textContent = latestSender1.minHtlc ?? '—';
    document.getElementById('stat-node1-max-htlc').textContent = latestSender1.maxHtlcMsat ?? '—';
    document.getElementById('stat-node1-cltv').textContent = latestSender1.timeLockDelta ?? '—';
    document.getElementById('stat-node1-disabled').textContent = latestSender1.disabled === '1' || latestSender1.disabled === 'True' ? 'Yes' : 'No';
  } else {
    document.getElementById('stat-node1-state').textContent = '—';
    document.getElementById('stat-node1-timestamp').textContent = '—';
    document.getElementById('stat-node1-date').textContent = '—';
    document.getElementById('stat-node1-fee-base').textContent = '—';
    document.getElementById('stat-node1-fee-rate').textContent = '—';
    document.getElementById('stat-node1-min-htlc').textContent = '—';
    document.getElementById('stat-node1-max-htlc').textContent = '—';
    document.getElementById('stat-node1-cltv').textContent = '—';
    document.getElementById('stat-node1-disabled').textContent = '—';
  }

  // Node 2
  if (latestSender2) {
    const state2 = latestSender2.disabled === '1' || latestSender2.disabled === 'True' ? 'Disabled' : 'Active';
    const date2 = latestSender2.recording_time && !isNaN(parseInt(latestSender2.recording_time))
      ? new Date(parseInt(latestSender2.recording_time) * 1000).toISOString().slice(0, 10)
      : '—';

    document.getElementById('stat-node2-state').textContent = state2;
    document.getElementById('stat-node2-timestamp').textContent = latestSender2.recording_time ?? '—';
    document.getElementById('stat-node2-date').textContent = date2;
    document.getElementById('stat-node2-fee-base').textContent = latestSender2.feeBaseMsat ?? '—';
    document.getElementById('stat-node2-fee-rate').textContent = latestSender2.feeRateMilliMsat ?? '—';
    document.getElementById('stat-node2-min-htlc').textContent = latestSender2.minHtlc ?? '—';
    document.getElementById('stat-node2-max-htlc').textContent = latestSender2.maxHtlcMsat ?? '—';
    document.getElementById('stat-node2-cltv').textContent = latestSender2.timeLockDelta ?? '—';
    document.getElementById('stat-node2-disabled').textContent = latestSender2.disabled === '1' || latestSender2.disabled === 'True' ? 'Yes' : 'No';
  } else {
    document.getElementById('stat-node2-state').textContent = '—';
    document.getElementById('stat-node2-timestamp').textContent = '—';
    document.getElementById('stat-node2-date').textContent = '—';
    document.getElementById('stat-node2-fee-base').textContent = '—';
    document.getElementById('stat-node2-fee-rate').textContent = '—';
    document.getElementById('stat-node2-min-htlc').textContent = '—';
    document.getElementById('stat-node2-max-htlc').textContent = '—';
    document.getElementById('stat-node2-cltv').textContent = '—';
    document.getElementById('stat-node2-disabled').textContent = '—';
  }

  const metaSection = document.getElementById('channel-meta');

  if (meta || edgeMeta) {
    const createdTs = edgeMeta?.created_timestamp || meta?.created;
    const created = createdTs && !isNaN(parseInt(createdTs))
      ? new Date(parseInt(createdTs) * 1000).toISOString().slice(0, 10)
      : '—';

    const closingTs = edgeMeta?.closing_timestamp || meta?.closing_date;
    const closed = (closingTs && closingTs !== '0' && !isNaN(parseInt(closingTs)))
      ? new Date(parseInt(closingTs) * 1000).toISOString().slice(0, 10)
      : 'Open';
    const capacitySats = (edgeMeta?.capacity || meta?.capacity)
      ? parseInt(edgeMeta?.capacity || meta?.capacity).toLocaleString() + ' sats'
      : '—';

    document.getElementById('meta-capacity').textContent = capacitySats;
    document.getElementById('meta-node1').textContent = edgeMeta?.node1Pub || meta?.node_1 || '—';
    document.getElementById('meta-node1').title = edgeMeta?.node1Pub || meta?.node_1 || '';
    document.getElementById('meta-node2').textContent = edgeMeta?.node2Pub || meta?.node_2 || '—';
    document.getElementById('meta-node2').title = edgeMeta?.node2Pub || meta?.node_2 || '';
    document.getElementById('meta-created').textContent = created;
    document.getElementById('meta-closed').textContent = closed;

    // Populate detailed metadata
    document.getElementById('detail-channel-id').textContent = edgeMeta?.channelId || channelId || '—';
    document.getElementById('detail-short-id').textContent = edgeMeta?.shortId || '—';
    document.getElementById('detail-funding-txid').textContent = edgeMeta?.fundingTxid || '—';
    document.getElementById('detail-funding-index').textContent = edgeMeta?.fundingOutputIndex || '—';

    const node1Pub = edgeMeta?.node1Pub || meta?.node_1 || '—';
    const node2Pub = edgeMeta?.node2Pub || meta?.node_2 || '—';

    if (node1Pub !== '—') {
      document.getElementById('detail-node1-pub').innerHTML = `<a class="node-pubkey-link" onclick="viewNodeFromChannel('${node1Pub}')">${node1Pub}</a>`;
    } else {
      document.getElementById('detail-node1-pub').textContent = '—';
    }

    if (node2Pub !== '—') {
      document.getElementById('detail-node2-pub').innerHTML = `<a class="node-pubkey-link" onclick="viewNodeFromChannel('${node2Pub}')">${node2Pub}</a>`;
    } else {
      document.getElementById('detail-node2-pub').textContent = '—';
    }
    document.getElementById('detail-capacity').textContent = edgeMeta?.capacity ? parseInt(edgeMeta.capacity).toLocaleString() + ' sats' : '—';

    const detailedCreatedTs = edgeMeta?.created_timestamp && !isNaN(parseInt(edgeMeta.created_timestamp))
      ? new Date(parseInt(edgeMeta.created_timestamp) * 1000).toISOString().replace('T', ' ').slice(0, 19) + ' UTC'
      : '—';
    document.getElementById('detail-created-ts').textContent = detailedCreatedTs;
    document.getElementById('detail-created-height').textContent = edgeMeta?.created_height || '—';

    const detailedClosingTs = (edgeMeta?.closing_timestamp && edgeMeta.closing_timestamp !== '0' && !isNaN(parseInt(edgeMeta.closing_timestamp)))
      ? new Date(parseInt(edgeMeta.closing_timestamp) * 1000).toISOString().replace('T', ' ').slice(0, 19) + ' UTC'
      : '—';
    document.getElementById('detail-closing-ts').textContent = detailedClosingTs;
    document.getElementById('detail-closing-height').textContent = edgeMeta?.closing_height || '—';

    metaSection.classList.remove('hidden');
  } else {
    metaSection.classList.add('hidden');
  }
}

function renderChannelCharts(channelId, rows) {
  // Sort by recording_time ascending for chart display
  const sorted = [...rows].sort((a, b) => parseInt(a.recording_time || 0) - parseInt(b.recording_time || 0));

  // Get node names from dictionary
  const meta = dictionary?.[channelId];

  // Determine node order by comparing pubkeys (smallest = 1)
  const node1Pub = meta?.node_1 || '';
  const node2Pub = meta?.node_2 || '';
  const isNode1Smaller = node1Pub < node2Pub;

  const node1Label = node1Pub ? node1Pub.slice(0, 20) + '…' : 'Node 1';
  const node2Label = node2Pub ? node2Pub.slice(0, 20) + '…' : 'Node 2';

  // Separate data by advertiser based on node order
  let sender1Data, sender2Data;
  if (isNode1Smaller) {
    sender1Data = sorted.filter(r => r.advertiser === '1');
    sender2Data = sorted.filter(r => r.advertiser === '2');
  } else {
    sender1Data = sorted.filter(r => r.advertiser === '2');
    sender2Data = sorted.filter(r => r.advertiser === '1');
  }

  const isLightMode = document.body.classList.contains('light-mode');
  const gridColor = isLightMode ? '#e8e8e8' : '#1a1a24';
  const textColor = isLightMode ? '#555' : '#666';
  const tooltipBg = isLightMode ? '#ffffff' : '#1a1a24';
  const tooltipBorder = isLightMode ? '#e0e0e0' : '#1e1e2e';
  const tooltipText = isLightMode ? '#333' : '#ccc';

  // Helper to create dataset for a sender
  const createDataset = (label, data, field, color1, color2) => {
    const datasets = [
      {
        label: node1Label,
        data: sender1Data.map(r => r[field]),
        borderColor: color1,
        backgroundColor: color1 + '19',
        borderWidth: 1.5,
        tension: 0.3,
        pointRadius: 0,
      },
    ];
    if (sender2Data.length > 0) {
      datasets.push({
        label: node2Label,
        data: sender2Data.map(r => r[field]),
        borderColor: color2,
        backgroundColor: color2 + '19',
        borderWidth: 1.5,
        tension: 0.3,
        pointRadius: 0,
      });
    }
    return datasets;
  };

  const dates = sender1Data.map(r => new Date(parseInt(r.recording_time || 0) * 1000).toISOString().slice(0, 10));

  // Chart 1: Fee Base
  renderChannelChart('chart-channel-fee-base', {
    labels: dates,
    datasets: createDataset('Fee Base', sender1Data, 'feeBaseMsat', '#f5a623', '#66bb6a'),
  }, gridColor, textColor, tooltipBg, tooltipBorder, tooltipText);

  // Chart 2: Fee Rate
  renderChannelChart('chart-channel-fee-rate', {
    labels: dates,
    datasets: createDataset('Fee Rate', sender1Data, 'feeRateMilliMsat', '#f5a623', '#66bb6a'),
  }, gridColor, textColor, tooltipBg, tooltipBorder, tooltipText);

  // Chart 3: Min HTLC
  renderChannelChart('chart-channel-min-htlc', {
    labels: dates,
    datasets: createDataset('Min HTLC', sender1Data, 'minHtlc', '#f5a623', '#66bb6a'),
  }, gridColor, textColor, tooltipBg, tooltipBorder, tooltipText);

  // Chart 4: Max HTLC
  renderChannelChart('chart-channel-max-htlc', {
    labels: dates,
    datasets: createDataset('Max HTLC', sender1Data, 'maxHtlcMsat', '#f5a623', '#66bb6a'),
  }, gridColor, textColor, tooltipBg, tooltipBorder, tooltipText);

  // Chart 5: CLTV Delta
  renderChannelChart('chart-channel-cltv', {
    labels: dates,
    datasets: createDataset('CLTV Delta', sender1Data, 'timeLockDelta', '#f5a623', '#66bb6a'),
  }, gridColor, textColor, tooltipBg, tooltipBorder, tooltipText);
}

function renderChannelChart(elementId, data, gridColor, textColor, tooltipBg, tooltipBorder, tooltipText) {
  const el = document.getElementById(elementId);
  if (!el) return; // Element doesn't exist, skip

  const ctx = el.getContext('2d');

  if (charts[elementId]) {
    charts[elementId].destroy();
  }

  // Get disabled regions from the currently loaded rows
  const sorted = [...window.currentChannelRows].sort((a, b) => parseInt(a.recording_time || 0) - parseInt(b.recording_time || 0));
  const disabledIndices = sorted
    .map((row, idx) => (row.disabled === '1' || row.disabled === 'True' ? idx : null))
    .filter(idx => idx !== null);

  // Create annotations for disabled regions
  const plugins = [];
  if (disabledIndices.length > 0) {
    plugins.push({
      id: 'disabledBackground',
      afterDatasetsDraw(chart) {
        const ctx = chart.ctx;
        const xScale = chart.scales.x;
        const yScale = chart.scales.y;
        const isLightMode = document.body.classList.contains('light-mode');
        const disabledColor = isLightMode ? 'rgba(224, 85, 85, 0.08)' : 'rgba(224, 85, 85, 0.12)';

        // Group consecutive disabled indices into ranges
        const ranges = [];
        let start = disabledIndices[0];
        let end = start;

        for (let i = 1; i < disabledIndices.length; i++) {
          if (disabledIndices[i] === end + 1) {
            end = disabledIndices[i];
          } else {
            ranges.push({ start, end });
            start = disabledIndices[i];
            end = start;
          }
        }
        ranges.push({ start, end });

        // Draw disabled regions
        ranges.forEach(range => {
          const startX = xScale.getPixelForValue(range.start);
          const endX = xScale.getPixelForValue(range.end);
          const width = Math.max(endX - startX, 2);

          ctx.fillStyle = disabledColor;
          ctx.fillRect(startX, yScale.top, width, yScale.bottom - yScale.top);
        });
      },
    });
  }

  charts[elementId] = new Chart(ctx, {
    type: 'line',
    data: data,
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: {
          labels: { color: textColor, font: { size: 12, weight: '500' } },
          padding: 15,
        },
        tooltip: {
          backgroundColor: tooltipBg,
          titleColor: '#f5a623',
          bodyColor: tooltipText,
          borderColor: tooltipBorder,
          borderWidth: 1,
          padding: 10,
          displayColors: true,
          font: { size: 11 },
        },
      },
      scales: {
        x: {
          grid: { color: gridColor, drawBorder: false },
          ticks: { color: textColor, font: { size: 11 } },
        },
        y: {
          grid: { color: gridColor },
          ticks: { color: textColor, font: { size: 11 } },
        },
      },
    },
    plugins: plugins,
  });
}

function renderTable(rows) {
  // Sort by lastUpdate (most recent first)
  const sorted = [...rows].sort((a, b) => {
    const timeA = parseInt(a.lastUpdate) || 0;
    const timeB = parseInt(b.lastUpdate) || 0;
    return timeB - timeA;
  });

  sorted.forEach(row => {
    const tr = document.createElement('tr');

    const date = row.recording_time
      ? new Date(parseInt(row.recording_time) * 1000).toISOString().replace('T', ' ').slice(0, 19) + ' UTC'
      : '—';

    const lastUpdateDate = row.lastUpdate && !isNaN(parseInt(row.lastUpdate))
      ? new Date(parseInt(row.lastUpdate) * 1000).toISOString().replace('T', ' ').slice(0, 19) + ' UTC'
      : '—';

    const isDisabled = row.disabled === '1';
    const disabledBadge = isDisabled
      ? '<span class="badge-disabled">Disabled</span>'
      : '<span class="badge-active">Active</span>';

    tr.innerHTML = `
      <td>${row.recording_time ?? '—'}</td>
      <td>${row.lastUpdate ?? '—'}</td>
      <td>${lastUpdateDate}</td>
      <td>${row.timeLockDelta ?? '—'}</td>
      <td>${row.minHtlc ?? '—'}</td>
      <td>${row.maxHtlcMsat ?? '—'}</td>
      <td class="highlight">${row.feeBaseMsat ?? '—'}</td>
      <td class="highlight">${row.feeRateMilliMsat ?? '—'}</td>
      <td>${disabledBadge}</td>
      <td>${row.advertiser ?? '—'}</td>
    `;

    tableBody.appendChild(tr);
  });
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// DOWNLOAD PAGE
// ═══════════════════════════════════════════════════════════════════════════════════════════════

async function loadDownloadData() {
  // Load file sizes
  try {
    const [dailyRes, channelsRes] = await Promise.all([
      fetch('data/daily_metrics.json', { method: 'HEAD' }),
      fetch('data/channels.csv', { method: 'HEAD' }),
    ]);

    const dailySize = formatBytes(parseInt(dailyRes.headers.get('content-length')) || 0);
    const channelsSize = formatBytes(parseInt(channelsRes.headers.get('content-length')) || 0);

    document.getElementById('size-daily-metrics').textContent = `(${dailySize})`;
    document.getElementById('size-channels').textContent = `(${channelsSize})`;
  } catch {
    document.getElementById('size-daily-metrics').textContent = '(size unknown)';
    document.getElementById('size-channels').textContent = '(size unknown)';
  }
}

function formatBytes(bytes) {
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return (bytes / Math.pow(k, i)).toFixed(1) + ' ' + sizes[i];
}

function downloadFile(url, filename) {
  fetch(url)
    .then(res => {
      if (!res.ok) throw new Error('Failed to download');
      return res.blob();
    })
    .then(blob => {
      const link = document.createElement('a');
      link.href = URL.createObjectURL(blob);
      link.download = filename;
      document.body.appendChild(link);
      link.click();
      document.body.removeChild(link);
      URL.revokeObjectURL(link.href);
    })
    .catch(err => {
      alert('Download failed: ' + err.message);
    });
}

document.getElementById('download-channel-form').addEventListener('submit', async e => {
  e.preventDefault();
  const channelId = document.getElementById('download-channel-input').value.trim();
  const errorEl = document.getElementById('download-error');

  if (!channelId) {
    errorEl.textContent = 'Please enter a channel ID';
    errorEl.classList.remove('hidden');
    return;
  }

  errorEl.classList.add('hidden');

  try {
    // Download channel data
    await downloadFile(DATA_PATHS.channels(channelId), `${channelId}.csv`);
    // Download channel metadata
    await downloadFile(DATA_PATHS.channelMetadata(channelId), `${channelId}_metadata.csv`);
  } catch {
    errorEl.textContent = `Failed to download channel ${channelId}`;
    errorEl.classList.remove('hidden');
  }
});

document.getElementById('download-node-form').addEventListener('submit', async e => {
  e.preventDefault();
  const nodePubKey = document.getElementById('download-node-input').value.trim();
  const errorEl = document.getElementById('download-node-error');

  if (!nodePubKey) {
    errorEl.textContent = 'Please enter a node public key';
    errorEl.classList.remove('hidden');
    return;
  }

  errorEl.classList.add('hidden');

  try {
    // Download all 4 node files
    const files = ['_addresses.csv', '_channels.csv', '_features.csv', '_status.csv'];
    for (const file of files) {
      const filePath = `data/nodes/${nodePubKey}${file}`;
      const fileName = `${nodePubKey}${file}`;
      await downloadFile(filePath, fileName);
    }
  } catch {
    errorEl.textContent = `Failed to download node ${nodePubKey}`;
    errorEl.classList.remove('hidden');
  }
});

// ═══════════════════════════════════════════════════════════════════════════════════════════════
// NODE EXPLORER
// ═══════════════════════════════════════════════════════════════════════════════════════════════

const nodeForm = document.getElementById('node-search-form');
const nodeInput = document.getElementById('node-input');
const nodeResults = document.getElementById('node-results');
const nodeErrorMsg = document.getElementById('node-error-msg');

nodeForm.addEventListener('submit', async e => {
  e.preventDefault();
  const nodePubKey = nodeInput.value.trim();
  if (!nodePubKey) return;
  await loadNode(nodePubKey);
});

async function loadNode(nodePubKey) {
  nodeErrorMsg.classList.add('hidden');
  nodeResults.classList.add('hidden');

  const nodePaths = {
    addresses: `data/nodes/${nodePubKey}_addresses.csv`,
    channels: `data/nodes/${nodePubKey}_channels.csv`,
    features: `data/nodes/${nodePubKey}_features.csv`,
    status: `data/nodes/${nodePubKey}_status.csv`,
  };

  try {
    const [addressText, channelsText, featuresText, statusText] = await Promise.all([
      fetch(nodePaths.addresses).then(r => r.ok ? r.text() : null),
      fetch(nodePaths.channels).then(r => r.ok ? r.text() : null),
      fetch(nodePaths.features).then(r => r.ok ? r.text() : null),
      fetch(nodePaths.status).then(r => r.ok ? r.text() : null),
    ]);

    if (!statusText) {
      nodeErrorMsg.textContent = `Node "${nodePubKey}" not found.`;
      nodeErrorMsg.classList.remove('hidden');
      return;
    }

    const addresses = parseCSV(addressText || '');
    const channels = parseCSV(channelsText || '');
    const features = parseCSV(featuresText || '');
    const status = parseCSV(statusText || '');

    renderNodeInfo(nodePubKey, status, addresses, channels, features);
    nodeResults.classList.remove('hidden');
  } catch (e) {
    console.error('Error loading node:', e);
    nodeErrorMsg.textContent = 'Error loading node data.';
    nodeErrorMsg.classList.remove('hidden');
  }
}

function renderNodeInfo(nodePubKey, statusRows, addressRows, channelRows, featureRows) {
  const latestStatus = statusRows.length > 0 ? statusRows[statusRows.length - 1] : null;
  const shortKey = nodePubKey.slice(0, 12) + '…';

  document.getElementById('node-pubkey-header').textContent = shortKey;
  document.getElementById('node-pubkey').textContent = nodePubKey;

  if (latestStatus) {
    document.getElementById('node-alias').textContent = latestStatus.alias ?? '—';

    const colorElement = document.getElementById('node-color');
    if (latestStatus.color) {
      colorElement.innerHTML = `<span style="display:inline-block;width:16px;height:16px;background-color:${latestStatus.color};border-radius:3px;vertical-align:middle;margin-right:0.5rem;"></span>${latestStatus.color}`;
    } else {
      colorElement.textContent = '—';
    }

    const lastUpdate = latestStatus.recording_time
      ? new Date(parseInt(latestStatus.recording_time) * 1000).toISOString().replace('T', ' ').slice(0, 19) + ' UTC'
      : '—';
    document.getElementById('node-last-update').textContent = lastUpdate;

    document.getElementById('node-meta').classList.remove('hidden');
  } else {
    document.getElementById('node-meta').classList.add('hidden');
  }

  document.getElementById('node-stat-addresses').textContent = addressRows.length.toLocaleString();
  document.getElementById('node-stat-channels').textContent = channelRows.length.toLocaleString();
  document.getElementById('node-stat-features').textContent = featureRows.length.toLocaleString();

  renderNodeAddresses(addressRows);
  renderNodeChannels(channelRows, nodePubKey);
  renderNodeFeatures(featureRows);
}

function renderNodeAddresses(rows) {
  const tbody = document.getElementById('node-addresses-table-body');
  tbody.innerHTML = '';

  rows.forEach(row => {
    const tr = document.createElement('tr');
    const date = row.recording_time
      ? new Date(parseInt(row.recording_time) * 1000).toISOString().replace('T', ' ').slice(0, 19) + ' UTC'
      : '—';

    tr.innerHTML = `
      <td>${row.recording_time ?? '—'}</td>
      <td>${row.lastUpdate ?? '—'}</td>
      <td>${row.network ?? '—'}</td>
      <td class="mono">${row.addr ?? '—'}</td>
      <td>${row.is_snapshot ?? '—'}</td>
    `;
    tbody.appendChild(tr);
  });
}

function renderNodeChannels(rows, nodePubKey) {
  const tbody = document.getElementById('node-channels-table-body');
  tbody.innerHTML = '';

  const relatedNodes = new Set();

  rows.forEach(row => {
    const tr = document.createElement('tr');
    const channelId = row.channelId ?? row.channel_id ?? '—';

    tr.innerHTML = `
      <td>${channelId}</td>
      <td><button class="channel-link-btn" onclick="viewChannelFromNode('${channelId}')">View Channel</button></td>
    `;
    tbody.appendChild(tr);

    // Extract other node from dictionary
    if (dictionary && dictionary[channelId]) {
      const meta = dictionary[channelId];
      if (meta.node_1 && meta.node_1 !== nodePubKey) {
        relatedNodes.add(meta.node_1);
      }
      if (meta.node_2 && meta.node_2 !== nodePubKey) {
        relatedNodes.add(meta.node_2);
      }
    }
  });

  renderRelatedNodes(Array.from(relatedNodes));
}

function renderRelatedNodes(nodeList) {
  const tbody = document.getElementById('node-related-nodes-table-body');
  tbody.innerHTML = '';

  nodeList.forEach(nodePubKey => {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td class="mono">${nodePubKey}</td>
      <td><button class="channel-link-btn" onclick="viewNodeFromNode('${nodePubKey}')">View Node</button></td>
    `;
    tbody.appendChild(tr);
  });
}

function renderNodeFeatures(rows) {
  const tbody = document.getElementById('node-features-table-body');
  tbody.innerHTML = '';

  rows.forEach(row => {
    const tr = document.createElement('tr');
    const date = row.recording_time
      ? new Date(parseInt(row.recording_time) * 1000).toISOString().slice(0, 10)
      : '—';

    tr.innerHTML = `
      <td>${date}</td>
      <td>${row.feature ?? '—'}</td>
      <td>${row.name ?? '—'}</td>
      <td>${row.isKnown === 'True' ? 'Yes' : 'No'}</td>
      <td>${row.isRequired === 'True' ? 'Yes' : 'No'}</td>
    `;
    tbody.appendChild(tr);
  });
}

function viewChannelFromNode(channelId) {
  // Switch to channel view and load the channel
  const channelTab = document.querySelector('[data-view="channel-view"]');
  channelTab.click();
  document.getElementById('channel-input').value = channelId;
  loadChannel(channelId);
}

function viewNodeFromNode(nodePubKey) {
  // Stay in node view but load the related node
  document.getElementById('node-input').value = nodePubKey;
  loadNode(nodePubKey);
}

function viewNodeFromChannel(nodePubKey) {
  // Switch to node view and load the node
  const nodeTab = document.querySelector('[data-view="node-view"]');
  nodeTab.click();
  document.getElementById('node-input').value = nodePubKey;
  loadNode(nodePubKey);
}
