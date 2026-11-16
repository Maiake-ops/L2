'use strict';
const announcement = document.querySelector('#announcement');
for (const button of document.querySelectorAll('.copy')) {
  button.hidden = false;
  button.addEventListener('click', async () => {
    const text = button.parentElement.querySelector('code').textContent;
    try {
      await navigator.clipboard.writeText(text);
      button.textContent = 'Copied';
      announcement.textContent = 'Code copied to clipboard.';
    } catch {
      const range = document.createRange();
      range.selectNodeContents(button.parentElement.querySelector('code'));
      const selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      button.textContent = 'Selected';
      announcement.textContent = 'Clipboard unavailable. Code selected; use your copy shortcut.';
    }
    setTimeout(() => { button.textContent = 'Copy'; }, 1800);
  });
}
const packageSearch = document.querySelector('#package-search');
if (packageSearch) {
  const kind = document.querySelector('#package-kind');
  const rows = [...document.querySelectorAll('[data-package]')];
  const filter = () => {
    const query = packageSearch.value.trim().toLowerCase();
    let count = 0;
    for (const row of rows) {
      row.hidden = !row.textContent.toLowerCase().includes(query) || (kind.value !== 'all' && row.dataset.kind !== kind.value);
      if (!row.hidden) count++;
    }
    document.querySelector('#package-count').textContent = `${count} of ${rows.length} recipes`;
    document.querySelector('#package-empty').hidden = count !== 0;
  };
  packageSearch.addEventListener('input', filter);
  kind.addEventListener('change', filter);
}
const docSearch = document.querySelector('#doc-search');
if (docSearch) {
  const sections = [...document.querySelectorAll('[data-doc-section]')];
  const links = [...document.querySelectorAll('[data-doc-link]')];
  const filter = () => {
    const query = docSearch.value.trim().toLowerCase();
    let count = 0;
    sections.forEach((section, index) => {
      const match = section.textContent.toLowerCase().includes(query);
      section.hidden = !match;
      links[index].hidden = !match;
      if (match) count++;
    });
    document.querySelector('#doc-empty').hidden = count !== 0;
    document.querySelector('#doc-status').textContent = query ? `${count} of ${sections.length} topics` : '';
  };
  docSearch.addEventListener('input', filter);
  // A deep link must remain visible after a search, including browser back/forward.
  window.addEventListener('hashchange', () => {
    if (docSearch.value) { docSearch.value = ''; filter(); }
    const target = document.getElementById(location.hash.slice(1));
    if (target) target.scrollIntoView();
  });
}
