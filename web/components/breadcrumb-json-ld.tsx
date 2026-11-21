type BreadcrumbJsonLdProps = {
  locale: "ru" | "en";
  name: string;
  path: string;
};

export function BreadcrumbJsonLd({
  locale,
  name,
  path,
}: BreadcrumbJsonLdProps) {
  const data = {
    "@context": "https://schema.org",
    "@type": "BreadcrumbList",
    itemListElement: [
      {
        "@type": "ListItem",
        position: 1,
        name: "VLT Studio",
        item: `https://vltstudio.ru/${locale}`,
      },
      {
        "@type": "ListItem",
        position: 2,
        name,
        item: `https://vltstudio.ru/${locale}${path}`,
      },
    ],
  };

  return (
    <script
      type="application/ld+json"
      dangerouslySetInnerHTML={{
        __html: JSON.stringify(data).replace(/</g, "\\u003c"),
      }}
    />
  );
}
