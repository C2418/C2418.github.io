import type {
        ExpressiveCodeConfig,
        LicenseConfig,
        NavBarConfig,
        ProfileConfig,
        SiteConfig,
} from "./types/config";


export const siteConfig: SiteConfig = {
        title: "2418",
        subtitle: "AI Assistant's favorite student",
        lang: "zh_CN",
        themeColor: {
                hue: 250,
                fixed: false,
        },
        banner: {
                enable: false,
                src: "assets/images/demo-banner.png",
                position: "center",
                credit: {
                        enable: false,
                        text: "",
                        url: "",
                },
        },
        toc: {
                enable: true,
                depth: 2,
        },
        favicon: [],
};

export const navBarConfig: NavBarConfig = {
        links: [
                {
                        name: "首页",
                        url: "/",
                },
                {
                        name: "归档",
                        url: "/archive/",
                },
                {
                        name: "关于",
                        url: "/about/",
                },
        ],
};

export const profileConfig: ProfileConfig = {
        avatar: "assets/images/avatar.jpg",
        name: "2418",
        bio: "AI Assistant's favorite student",
        links: [
                {
                        name: "GitHub",
                        icon: "fa6-brands:github",
                        url: "https://github.com/C2418",
                },
        ],
};

export const licenseConfig: LicenseConfig = {
        enable: true,
        name: "CC BY-NC-SA 4.0",
        url: "https://creativecommons.org/licenses/by-nc-sa/4.0/",
};

export const expressiveCodeConfig: ExpressiveCodeConfig = {
        theme: "github-dark",
};
