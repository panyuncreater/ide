// 将 minilang_architecture.svg 转为 PNG，供 docx 嵌入。
// 依赖：puppeteer（随 mermaid-cli 全局安装，ESM 模块，用动态 import 加载）。
// 用法：node scripts\svg_to_png.js
const path = require('path');
const { pathToFileURL } = require('url');

(async () => {
  const globalRoot = require('child_process')
    .execSync('npm root -g', { encoding: 'utf8' })
    .trim();
  const puppeteerPath = path.join(
    globalRoot, '@mermaid-js', 'mermaid-cli', 'node_modules', 'puppeteer', 'lib', 'puppeteer', 'puppeteer.js'
  );
  const puppeteerNs = await import(pathToFileURL(puppeteerPath).href);
  const puppeteer = puppeteerNs.default || puppeteerNs;

  const root = path.resolve(__dirname, '..');
  const svgPath = path.join(root, 'minilang_architecture.svg');
  const pngPath = path.join(root, 'minilang_architecture.png');
  const browser = await puppeteer.launch({ headless: 'new', args: ['--no-sandbox'] });
  try {
    const page = await browser.newPage();
    await page.setViewport({ width: 1600, height: 1200, deviceScaleFactor: 2 });
    const url = pathToFileURL(svgPath).href;
    await page.goto(url, { waitUntil: 'networkidle0' });
    const dims = await page.evaluate(() => {
      const svg = document.querySelector('svg');
      if (svg) {
        const r = svg.getBoundingClientRect();
        return { w: Math.max(r.width, 800), h: Math.max(r.height, 600) };
      }
      return { w: 1600, h: 1200 };
    });
    await page.setViewport({
      width: Math.ceil(dims.w),
      height: Math.ceil(dims.h),
      deviceScaleFactor: 2,
    });
    await page.screenshot({ path: pngPath, omitBackground: false });
    console.log('Done:', pngPath, dims);
  } finally {
    await browser.close();
  }
})();
