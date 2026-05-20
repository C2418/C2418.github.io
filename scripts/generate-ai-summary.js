import fs from 'fs';
import path from 'path';
import matter from 'gray-matter';
import OpenAI from 'openai';

const openai = new OpenAI({
    apiKey: process.env.OPENAI_API_KEY,
    baseURL: process.env.OPENAI_BASE_URL || 'https://api.openai.com/v1',
});

const CONTENT_DIR = 'src/content/posts';

async function generateSummary(content) {
    const prompt = `请为以下博客文章生成一段简洁的中文摘要，长度在 80 字以内。内容要吸引人且准确。文章内容如下：\n\n${content.substring(0, 2000)}`;
    
    try {
        const response = await openai.chat.completions.create({
            model: 'gpt-3.5-turbo',
            messages: [{ role: 'user', content: prompt }],
            max_tokens: 150,
        });
        return response.choices[0].message.content.trim();
    } catch (error) {
        console.error('OpenAI Error:', error.message);
        return null;
    }
}

async function processFiles() {
    const files = fs.readdirSync(CONTENT_DIR).filter(f => f.endsWith('.md') || f.endsWith('.mdx'));
    
    for (const file of files) {
        const filePath = path.join(CONTENT_DIR, file);
        const fileContent = fs.readFileSync(filePath, 'utf8');
        const { data, content } = matter(fileContent);
        
        if (!data.description && !data.ai_summary) {
            console.log(`Generating summary for: ${file}...`);
            const summary = await generateSummary(content);
            if (summary) {
                data.description = summary;
                const updatedContent = matter.stringify(content, data);
                fs.writeFileSync(filePath, updatedContent, 'utf8');
                console.log(`✅ Success: ${file}`);
            }
        } else {
            console.log(`Skipping: ${file} (Summary exists)`);
        }
    }
}

processFiles().catch(err => console.error(err));
