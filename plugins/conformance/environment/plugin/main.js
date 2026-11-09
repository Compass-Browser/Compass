async function fetchItems(config, c) {
  const same = c === compass;
  const doc = compass.xml.parse('<a><b x="1">t</b></a>');
  return {items: [{
    id: 'env',
    title: [compass.api, same, compass.locale, compass.timeZone, new Date(compass.now()).toISOString(),
            doc.find('b').attr('x'), compass.secret('nope'), typeof __compassHost].join(' '),
    url: 'https://example.com/',
  }]};
}
