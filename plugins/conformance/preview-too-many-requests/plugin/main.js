// Conformance case: see ../../README.md.
async function previewTab(page) {
  for (let i = 0; i < 6; i++) await compass.fetch('https://example.com/n');
  return {items: []};
}
