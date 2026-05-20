const fs = require('fs');

function safeWrite(filePath, content) {
    fs.writeFileSync(filePath, content, 'utf8');
    console.log(`Updated: ${filePath}`);
}

// 1. Fix [...page].astro encoding and text
const pagePath = 'D:/blog/src/pages/[...page].astro';
let pageContent = fs.readFileSync(pagePath, 'utf8');
// Fix the garbled button text
pageContent = pageContent.replace(/<a href="\/archive\/" class="btn-plain scale-animation rounded-lg h-11 px-5 font-bold active:scale-95 flex items-center gap-2">[\s\S]*?<Icon/, 
    `<a href="/archive/" class="btn-plain scale-animation rounded-lg h-11 px-5 font-bold active:scale-95 flex items-center gap-2">
                    前往归档 <Icon`);
safeWrite(pagePath, pageContent);

// 2. Fix config.ts encoding and text
const configPath = 'D:/blog/src/config.ts';
let configContent = fs.readFileSync(configPath, 'utf8');
configContent = configContent.replace(/name: "妫ｆ牠銆"/, 'name: "首页"');
configContent = configContent.replace(/name: "瑜版帗銆"/, 'name: "归档"');
configContent = configContent.replace(/name: "閸忓厖绨"/, 'name: "关于"');
// Fallback for the current garbled state if the above fails
configContent = configContent.replace(/url: "\/",\s+},\s+{\s+name: ".*?",/, 'url: "/",\n                },\n                {\n                        name: "归档",');
configContent = configContent.replace(/url: "\/archive\/",\s+},\s+{\s+name: ".*?",/, 'url: "/archive/",\n                },\n                {\n                        name: "关于",');
safeWrite(configPath, configContent);

// 3. Fix archive.astro just in case
const archivePath = 'D:/blog/src/pages/archive.astro';
if (fs.existsSync(archivePath)) {
    let archiveContent = fs.readFileSync(archivePath, 'utf8');
    // Ensure no garbled characters in imports or layout titles
    safeWrite(archivePath, archiveContent);
}
