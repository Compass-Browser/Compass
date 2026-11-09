async function fetchItems() {
  const items = [];
  for (let i = 0; i < 5; i++) items.push({id: 'i' + i, title: 'Item ' + i, url: 'https://example.com/' + i});
  return {items};
}
