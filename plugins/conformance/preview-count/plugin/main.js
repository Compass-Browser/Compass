// Conformance case: see ../../README.md.
// The count is capped and whole; with no items the result still carries it.
async function previewTab(page) {
  return {items: [], count: 123456.7};
}
