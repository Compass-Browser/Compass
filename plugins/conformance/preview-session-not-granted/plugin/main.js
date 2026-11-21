// Conformance case: see ../../README.md.
async function previewTab(page) {
  if (compass.hasSession) throw new Error('hasSession must be false');
  await compass.fetch('https://example.com/data');
  return {items: []};
}
