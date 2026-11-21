// Conformance case: see ../../README.md.
// Sees only the declared groups, capped, and only the declared cookies, and
// only with the session granted. The snapshot is frozen.
async function previewTab(page) {
  let frozen = false;
  try { page.groups.rows.push({}); } catch (e) { frozen = true; }
  return {items: [{id: 's', title: JSON.stringify({groups: Object.keys(page.groups), rows: page.groups.rows.length,
                                                     cookies: page.cookies || null, frozen})}]};
}
