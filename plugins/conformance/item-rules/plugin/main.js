async function fetchItems() {
  return {
    title: '   Folder   title  ',
    nextRefresh: 120.4,
    items: [
      {id: 1, title: '  Numeric   id ', url: 'https://example.com/1', time: '2026-11-09T10:00:00Z', badge: 'A very long badge'},
      {id: '1', title: 'Duplicate id', url: 'https://example.com/dup'},
      {id: 'js', title: 'Script URL', url: 'javascript:alert(1)'},
      {id: 'rel', title: 'Relative URL', url: '/relative'},
      {id: 'notitle', title: '   ', url: 'https://example.com/x'},
      {id: '', title: 'Empty id', url: 'https://example.com/y'},
      null,
      {id: 'icon', title: 'Bad icon and time', url: 'https://example.com/z', icon: 'data:image/png;base64,xx', time: 'soon', subtitle: 'kept'},
      {id: 'extra', title: 'Unknown fields dropped', url: 'https://example.com/e', secret: 'x'},
    ],
  };
}
