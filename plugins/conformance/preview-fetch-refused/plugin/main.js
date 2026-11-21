// Conformance case: see ../../README.md.
async function previewTab(page) {
  await compass.fetch('https://example.com/data');
  return {items: []};
}
