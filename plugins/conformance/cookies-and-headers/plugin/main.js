async function fetchItems() {
  await compass.fetch('https://a.example.com/', {credentials: 'include', headers: {cookie: 'forged=1'}});
  await compass.fetch('https://b.example.com/', {credentials: 'include'});
  await compass.fetch('https://a.example.com/omit');
  return {items: []};
}
