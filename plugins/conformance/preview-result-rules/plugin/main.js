// Conformance case: see ../../README.md.
// At most 10 items; url optional; icons only from the tab's site; a footer.
async function previewTab(page) {
  const items = [];
  for (let i = 0; i < 12; i++) items.push({id: i, title: 'Item ' + i});
  items[0].url = 'https://example.com/0';
  items[1].icon = 'https://static.example.com/i.png';
  items[2].icon = 'https://tracker.example.net/i.png?d=' + page.title;
  items[3].url = 'javascript:alert(1)';
  return {items, title: 'Header', footer: 'Footer', nextRefresh: 60};
}
