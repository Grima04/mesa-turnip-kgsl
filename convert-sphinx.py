import os, glob
from bs4 import BeautifulSoup
from subprocess import run, PIPE
from urllib.parse import urlparse
import dashtable

def html_to_rst(input):
    return run(['pandoc', '-f', 'html', '-t', 'rst'],
               input=input, stdout=PIPE, universal_newlines=True).stdout

def convert_toc(filename):
    with open(filename, encoding='utf8') as input:
        soup = BeautifulSoup(input, 'html5lib')
        body = soup.find('body')
        with open('./docs/contents.rst', 'w', encoding='utf-8') as output:
            for elm in body.contents:
                if elm.name == 'h2':
                    output.write(""".. toctree::
   :maxdepth: 1
   :caption: {0}
   :hidden:\n""".format(elm.get_text()))
                elif elm.name == 'ul':
                    output.write('\n')
                    for li in elm.contents:
                        if li.name == 'li':
                            a = li.find('a')
                            url = a['href']
                            if url == 'index.html':
                                output.write('   self\n')
                            elif bool(urlparse(url).netloc):
                                output.write('   {0} <{1}>\n'.format(a.get_text(), url))
                            else:
                                output.write('   {0}\n'.format(url[:-5]))
                    output.write('\n')
                elif elm.name == 'dl':
                    a = elm.find('a')
                    output.write('\n   {0} <{1}>\n'.format(a.get_text(), url))
                elif hasattr(elm, 'contents'):
                    print('**** UNKNOWN: ' + str(elm))
                    exit(1)
    print("SUCCESS: " + filename)

def convert_article(filename):
    with open(filename, encoding='utf8') as input:
        soup = BeautifulSoup(input, 'html5lib')

        table = None
        if filename == './docs/release-calendar.html':
            table = dashtable.html2rst(str(soup.table.extract()))

        content = soup.find('div', 'content')
        content = ''.join(map(str, content.contents))
        content = html_to_rst(str(content))

        if table:
            content = '\n'.join([content, table, ''])

        with open(os.path.splitext(filename)[0]+'.rst', 'w', encoding='utf-8') as output:
            output.write(str(content))
            if filename == './docs/relnotes.html':
                output.write("""\n.. toctree::
   :maxdepth: 1
   :hidden:\n""")
                output.write('\n')
                for li in soup.findAll('li'):
                    a = li.find('a')
                    url = a['href']
                    split = os.path.splitext(url)
                    if split[1] == '.html':
                        output.write('   {0}\n'.format(split[0]))
                output.write('   Older Versions <versions>\n')

    print("SUCCESS: " + filename)

for filename in glob.iglob('./docs/**/*.html', recursive=True):
    if filename == './docs/contents.html':
        convert_toc(filename)
    else:
        convert_article(filename)
