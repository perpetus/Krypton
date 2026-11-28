import {sortableTable} from './ui.js';

for (const table of document.querySelectorAll('table[data-sortable]')) {
  sortableTable(table);
}
