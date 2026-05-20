/** @type {import('tailwindcss').Config} */
const defaultTheme = require("tailwindcss/defaultTheme")
module.exports = {
  content: ["./src/**/*.{astro,html,js,jsx,md,mdx,svelte,ts,tsx,vue,mjs}"],
  darkMode: "class",
  theme: {
    extend: {
      fontFamily: {
        sans: ["Roboto", "sans-serif", ...defaultTheme.fontFamily.sans],
      },
      colors: {
        accent: "var(--accent-color)",
      },
      backgroundImage: {
        'dot-grid': "radial-gradient(circle, var(--dot-color) 1px, transparent 1px)",
      },
      backgroundSize: {
        'dot-size': '20px 20px',
      }
    },
  },
  plugins: [require("@tailwindcss/typography")],
}
