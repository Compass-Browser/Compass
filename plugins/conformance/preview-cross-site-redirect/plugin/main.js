// Conformance case: see ../../README.md.
async function previewTab(page) {
  await compass.fetch('https://app.example.com/go');
  return {items: []};
}
