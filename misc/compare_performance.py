import argparse
import sys
import subprocess
import json, requests


__author__ = 'buchholb'

virtuoso_run_binary = "/home/buchholb/virtuoso/bin/isql"
virtuoso_isql_port = "1111"
bifc_isql_port = "1113"
bifc_inc_isql_port = "1114"
virtuso_isql_user = "dba"
rdf3x_run_binary = "/home/buchholb/rdf3x-0.3.8/bin/rdf3xquery"
rdf3x_db = "/local/scratch/bjoern/data/rdf3x//wikipedia-freebase.combined.db"
my_index = "/local/scratch/bjoern/data/wikipedia-freebase"
my_binary = "/local/scratch/bjoern/work/tmp/SparqlEngineMain"
broccoli_api = 'http://filicudi.informatik.uni-freiburg.de:6001/'

parser = argparse.ArgumentParser()

parser.add_argument('--queryfile',
                    type=str,
                    help='One line per (standard) SPARQL query. TS specific transformations are made by the python script.',
                    required=True)

parser.add_argument('--virtuoso-pwd',
                    type=str,
                    help='Password for the (already running) virtuoso instance',
                    required=True)


def expanded_to_my_syntax(q):
    before_where, after_where = q.split('WHERE')
    no_mod, mod = after_where.strip().split('}')
    clauses = no_mod.strip('{').split('.')
    new_clauses = []
    context_to_words = {}
    for c in clauses:
        if '<word:' in c:
            try:
                s, p, o = c.strip().split(' ')
                if o not in context_to_words:
                    context_to_words[o] = []
                context_to_words[o].append(s[6: -1])
            except ValueError:
                print("Problem in : " + c, file=sys.stderr)
                exit(1)
        else:
            new_clauses.append(c)
    for c, ws in context_to_words.items():
        new_clauses.append(' ' + c + ' <in-context> ' + ' '.join(ws) + ' ')
    new_after_where = ' {' + '.'.join(new_clauses) + '}' + mod
    return 'WHERE'.join([before_where, new_after_where])


def rewrite_for_rdf3x(q):
    if '*>' in q:
        print('Inexpressible prefix search in rdf3x: ' + q.strip(), file=sys.stderr)
        return 'NOT POSSIBLE: ' + q.strip()
    before_where, after_where = q.split('WHERE')
    if '<in-context>' in q and 'DISTINCT' not in q:
        before_where = before_where[:before_where.find('SELECT')] \
                       + ' SELECT DISTINCT' \
                       + before_where[before_where.find('SELECT') + 6:]
    no_mod, mod = after_where.strip().split('}')
    clauses = no_mod.strip('{').split('.')
    new_clauses = []
    for c in clauses:
        if 'FILTER' in c and '!=' in c:
            e1 = c[c.find('?'): c.find('!=')].strip()
            e2 = c[c.find('!=') + 2: c.find(')')].strip()
            new_clauses.append(' FILTER(!(' + e1 + '=' + e2 + ')) ')
        else:
            new_clauses.append(c)
    if 'OFFSET' in mod:
        print('Inexpressible in OFFSET clause in rdf3x: ' + mod, file=sys.stderr)
        return 'NOT POSSIBLE: ' + q.strip()
    if 'ORDER BY' in mod:
        mod = mod.replace('(', ' ')
        mod = mod.replace(')', '')
    new_after_where = ' {' + '.'.join(new_clauses) + '}' + mod
    return 'WHERE'.join([before_where, new_after_where])


def rewrite_to_bif_contains_inc(q):
    before_where, after_where = q.split('WHERE')
    if '<in-context>' in q and 'DISTINCT' not in q:
        before_where = before_where[:before_where.find('SELECT')] \
                       + ' SELECT DISTINCT' \
                       + before_where[before_where.find('SELECT') + 6:]
    no_mod, mod = after_where.strip().split('}')
    clauses = no_mod.strip('{').split('.')
    new_clauses = []
    context_to_words = {}
    for c in clauses:
        if '<word:' in c:
            try:
                s, p, o = c.strip().split(' ')
                if o not in context_to_words:
                    context_to_words[o] = []
                word = s[6: -1]
                if word[-1] == '*' or word.isdigit():
                    word = "'" + word + "'"
                context_to_words[o].append(word)
            except ValueError:
                print("Problem in : " + c, file=sys.stderr)
                exit(1)
        else:
            new_clauses.append(c)
    for c, ws in context_to_words.items():
        new_clauses.append(' ' + c + ' <text> ?text' + c[1:] + ' ')
        new_clauses.append(' ?text' + c[1:] + ' bif:contains "'
                           + ' and '.join(ws) + '" ')
    new_after_where = ' {' + '.'.join(new_clauses) + '}' + mod
    return 'WHERE'.join([before_where, new_after_where])


def rewrite_to_bif_contains(q):
    before_where, after_where = q.split('WHERE')
    if '<in-context>' in q and 'DISTINCT' not in q:
        before_where = before_where[:before_where.find('SELECT')] \
                       + ' SELECT DISTINCT' \
                       + before_where[before_where.find('SELECT') + 6:]
    no_mod, mod = after_where.strip().split('}')
    clauses = no_mod.strip('{').split('.')
    new_clauses = []
    context_to_words = {}
    entities_to_contexts = {}
    for c in clauses:
        if '<word:' in c:
            try:
                s, p, o = c.strip().split(' ')
                if o not in context_to_words:
                    context_to_words[o] = []
                word = s[6: -1]
                if word[-1] == '*' or word.isdigit():
                    word = "'" + word + "'"
                context_to_words[o].append(word)
            except ValueError:
                print("Problem in : " + c, file=sys.stderr)
                exit(1)
        else:
            if '<in-context>' not in c:
                new_clauses.append(c)
            else:
                try:
                    s, p, o = c.strip().split(' ')
                    if s[0] != '?':
                        print('Inexpressible in bif:c without in-c: ' + c, file=sys.stderr)
                        return 'NOT POSSIBLE: ' + q.strip()
                    if s not in entities_to_contexts:
                        entities_to_contexts[s] = []
                    entities_to_contexts[s].append(o)
                    times_on_rhs = 0
                    for e, cs in entities_to_contexts.items():
                        if o in cs:
                            times_on_rhs += 1
                    if times_on_rhs > 1:
                        print('Inexpressible in bif:c without in-c: ' + q.strip(), file=sys.stderr)
                        return 'NOT POSSIBLE: ' + q.strip()
                except ValueError:
                    print("Problem in : " + c, file=sys.stderr)
                    exit(1)
    for e, cs in entities_to_contexts.items():
        for i, c in enumerate(cs):
            new_clauses.append(' ' + e + ' <text> ?text' + str(i) + ' ')
            new_clauses.append(' ?text' + str(i) + ' bif:contains "' + ' and '.join(context_to_words[c]) + '" ')
    new_after_where = ' {' + '.'.join(new_clauses) + '}' + mod
    return 'WHERE'.join([before_where, new_after_where])


def rewrite_for_broccoli(q):
    before_where, after_where = q.split('WHERE')
    if before_where.count('?') > 1:
        print("Inexpressible in Broccoli: " + q.strip(), file=sys.stderr)
        return "NOT POSSIBLE: " + q.strip()
    i = before_where.find('?')
    j = before_where.find(' ', i)
    var_to_var = dict()
    select_var = before_where[i:j]
    var_to_var[select_var] = '$1'
    after_where = after_where.replace(select_var, '$1')
    j = 0
    while '?' in after_where:
        i = after_where.find('?', j)
        j = after_where.find(' ', i)
        sparql_var = after_where[i:j]
        bro_var = '$' + str(len(var_to_var) + 1)
        var_to_var[sparql_var] = bro_var
        after_where = after_where.replace(sparql_var, bro_var)
    no_mod, mod = after_where.strip().split('}')
    if 'FILTER' in after_where or 'ORDER' in mod:
        print("Inexpressible in Broccoli: " + q.strip(), file=sys.stderr)
        return "NOT POSSIBLE: " + q.strip()
    clauses = no_mod.strip('{').split('.')
    new_clauses = []
    context_to_words = {}
    context_to_entities = {}
    for c in clauses:
        try:
            s, p, o = c.strip().split(' ')
            if p == '<in-context>':
                if '<word' in s:
                    if o not in context_to_words:
                        context_to_words[o] = []
                    context_to_words[o].append(s[6: -1])
                else:
                    if o not in context_to_entities:
                        context_to_entities[o] = []
                    context_to_entities[o].append(s)
            else:
                if p[0] != '<':
                    print("Inexpressible in Broccoli: " + c, file=sys.stderr)
                    return "NOT POSSIBLE: " + q.strip()
                p = ':r:' + p[1:-1]
                if s[0] == '<':
                    var_to_var[s] = '$' + str(len(var_to_var) + 1)
                    entity = s[1:-1]
                    s = var_to_var[s]
                    new_clauses.append(s + " :r:equals :e:" + entity)
                if o[0] == '<':
                    if p == ':r:is-a':
                        o = ':e:' + o[1:-1]
                    else:
                        var_to_var[o] = '$' + str(len(var_to_var) + 1)
                        entity = o[1:-1]
                        o = var_to_var[o]
                        new_clauses.append(o + " :r:equals :e:" + entity)
                new_clauses.append(' '.join([s, p, o]))
        except ValueError:
            print("Problem in : " + c, file=sys.stderr)
            exit(1)
    if len(context_to_entities) > 0 and len(context_to_words) == 0:
        print("Inexpressible in Broccoli: " + c, file=sys.stderr)
        return "NOT POSSIBLE: " + q.strip()
    for c, es in context_to_entities.items():
        new_clauses.append(es[0] + ' :r:occurs-with '
                           + ' '.join(context_to_words[c]) + ' '
                           + ' '.join(es[1:]))
    for c, ws in context_to_words.items():
        if c not in context_to_entities:
            new_clauses.append('$1 :r:has-occurrence-of ' + ' '.join(ws))

    new_after_where = ';'.join(new_clauses)
    broccoli_mod = '&nofrelations=0&nofhitgroups=0&nofclasses=0'
    if 'LIMIT' in mod:
        i = mod.find('LIMIT')
        limit = mod[i+6:mod.find(' ', i + 6)]
        broccoli_mod += '&nofinstances=' + limit
    else:
        broccoli_mod += '&nofinstances=999999'
    if 'OFFSET' in mod:
        i = mod.find('OFFSET')
        offset = mod[i+6:mod.find(' ', i + 7)]
        broccoli_mod += '&firstinstance=' + offset
    return '?s=' + new_after_where.strip() + '&query=$1' + broccoli_mod




def get_virtuoso_query_times(query_file, pwd):
    with open('__tmp.virtqueries', 'w') as tmpfile:
        for line in open(query_file):
            tmpfile.write('SPARQL ' + line.strip().split('\t')[1] + ';\n')
    virtout = subprocess.check_output(
        [virtuoso_run_binary, virtuoso_isql_port, virtuso_isql_user, pwd,
         '__tmp.virtqueries']).decode('utf-8')
    coutfile = open('__tmp.cout.virtuoso', 'w')
    coutfile.write(virtout)
    coutfile.write('\n\n\n\n')
    times = []
    nof_matches = []
    for line in virtout.split('\n'):
        i = line.find('Rows. --')
        if i >= 0:
            j = line.find('msec')
            times.append(line[i + 9: j + 2])
            nof_matches.append(int(line[:i]))
            # os.remove('__tmp.rdf3xqueries')
    queries = []
    for line in open(query_file):
        queries.append(line.strip())
    if len(times) != len(queries) or len(times) != len(nof_matches):
        print('PROBLEM PROCESSING VIRTUOSO: q:' + str(len(queries)) + ' t:'
              + str(len(times)) + ' #:' + str(len(nof_matches)),
              file=sys.stderr)
    return times, nof_matches


def get_virtuoso_bifc_query_times(query_file, pwd):
    impossibles = {}
    with open('__tmp.bifc_queries', 'w') as tmpfile:
        i = 0
        for line in open(query_file):
            bifc_query = rewrite_to_bif_contains(line.strip().split('\t')[1])
            if 'NOT POSSIBLE:' in bifc_query:
                impossibles[i] = True
            else:
                tmpfile.write('SPARQL ' + bifc_query + ';\n')
            i += 1
    virtout = subprocess.check_output(
        [virtuoso_run_binary, bifc_isql_port, virtuso_isql_user, pwd,
         '__tmp.bifc_queries']).decode('utf-8')
    coutfile = open('__tmp.cout.bifc', 'w')
    coutfile.write(virtout)
    coutfile.write('\n\n\n\n')
    times = []
    nof_matches = []
    for line in virtout.split('\n'):
        i = line.find('Rows. --')
        if i >= 0:
            while len(times) in impossibles:
                times.append('-')
                nof_matches.append(0)
            j = line.find('msec')
            times.append(line[i + 9: j + 2])
            nof_matches.append(int(line[:i]))
            # os.remove('__tmp.bifc_queries')
    queries = []
    while len(times) in impossibles:
        times.append('-')
        nof_matches.append(0)
    for line in open(query_file):
        queries.append(line.strip())
    if len(times) != len(queries) or len(times) != len(nof_matches):
        print('PROBLEM PROCESSING VIRTUOSO: q:' + str(len(queries)) + ' t:' +
              str(len(times)) + ' #:' + str(len(nof_matches)), file=sys.stderr)
    return times, nof_matches


def get_virtuoso_bifc_inc_query_times(query_file, pwd):
    with open('__tmp.bifc_inc_queries', 'w') as tmpfile:
        for line in open(query_file):
            bifc_query = rewrite_to_bif_contains_inc(line.strip().split('\t')[1])
            tmpfile.write('SPARQL ' + bifc_query + ';\n')
    virtout = subprocess.check_output(
        [virtuoso_run_binary, bifc_inc_isql_port, virtuso_isql_user, pwd,
         '__tmp.bifc_inc_queries']).decode('utf-8')
    coutfile = open('__tmp.cout.bifc_inc', 'w')
    coutfile.write(virtout)
    coutfile.write('\n\n\n\n')
    times = []
    nof_matches = []
    for line in virtout.split('\n'):
        i = line.find('Rows. --')
        if i >= 0:
            j = line.find('msec')
            times.append(line[i + 9: j + 2])
            nof_matches.append(int(line[:i]))
            # os.remove('__tmp.bifc_inc_queries')
    queries = []
    for line in open(query_file):
        queries.append(line.strip())
    if len(times) != len(queries) or len(times) != len(nof_matches):
        print('PROBLEM PROCESSING VIRTUOSO: q:' + str(len(queries)) + ' t:' +
              str(len(times)) + ' #:' + str(len(nof_matches)), file=sys.stderr)
    return times, nof_matches


def get_rdf3X_query_times(query_file):
    impossibles = {}
    with open('__tmp.rdf3x_queries', 'w') as tmpfile:
        i = 0
        for line in open(query_file):
            rdf3x_query = rewrite_for_rdf3x(line.strip().split('\t')[1])
            if 'NOT POSSIBLE:' in rdf3x_query:
                impossibles[i] = True
            else:
                tmpfile.write(rdf3x_query + '\n')
            i += 1
    rdf3xout = subprocess.check_output(
        [rdf3x_run_binary, rdf3x_db, '__tmp.rdf3x_queries']).decode('utf-8')
    coutfile = open('__tmp.cout.rdf3x', 'w')
    coutfile.write(rdf3xout)
    coutfile.write('\n\n\n\n')
    times = []
    nof_matches = []
    last_ln = 0
    for ln, line in enumerate(rdf3xout.split('\n')):
        i = line.find('Done. Time: ')
        if i >= 0:
            while len(times) in impossibles:
                times.append('-')
                nof_matches.append(0)
            j = line.find('ms')
            times.append(line[i + 12: j + 2])
            nof_matches.append(ln - last_ln - 1)
            last_ln = ln + 1
    while len(times) in impossibles:
        times.append('-')
        nof_matches.append(0)
    # os.remove('__tmp.rdf3x_queries')
    queries = []
    for line in open(query_file):
        queries.append(line.strip())
    if len(times) != len(queries) or len(times) != len(nof_matches):
        print('PROBLEM PROCESSING RDF3X: q:' + str(len(queries)) + ' t:' +
                str(len(times)) + ' #:' + str(len(nof_matches)), file=sys.stderr)
    return times, nof_matches


def get_my_query_times(query_file):
    with open('__tmp.my_queries', 'w') as tmpfile:
        for line in open(query_file):
            try:
                tmpfile.write(
                    expanded_to_my_syntax(line.strip().split('\t')[1]) + '\n')
            except IndexError:
                print("Problem with tabs in : " + line)
                exit(1)
    coutfile = open('__tmp.cout.mine', 'w')
    myout = subprocess.check_output(
        [my_binary, '-i', my_index, '-t', '--queryfile', '__tmp.my_queries']).decode('utf-8')
    coutfile.write(myout)
    coutfile.write('\n\n\n\n')
    times = []
    nof_matches_no_limit = []
    nof_matches_limit = []
    for line in myout.split('\n'):
        i = line.find('Done. Time: ')
        if i >= 0:
            j = line.find('ms')
            times.append(line[i + 12: j + 2])
        i = line.find('Number of matches (no limit): ')
        if i >= 0:
            nof_matches_no_limit.append(int(line[i + 30:]))
        i = line.find('Number of matches (limit): ')
        if i >= 0:
            nof_matches_limit.append(int(line[i + 27:]))
    # os.remove('__tmp.my_queries')
    queries = []
    for line in open(query_file):
        queries.append(line.strip())
    if len(times) != len(queries) or len(times) != len(nof_matches_limit):
        print('PROBLEM PROCESSING MINE: q:' + str(len(queries)) + ' t:' +
                str(len(times)) + ' #:' + str(len(nof_matches_limit)), file=sys.stderr)
    return times, nof_matches_limit


def get_broccoli_query_times(query_file):
    impossibles = {}
    with open('__tmp.broccoli_queries', 'w') as tmpfile:
        i = 0
        for line in open(query_file):
            broccoli_query = rewrite_for_broccoli(line.strip().split('\t')[1])
            if 'NOT POSSIBLE:' in broccoli_query:
                impossibles[i] = True
            else:
                tmpfile.write(broccoli_query + '\n')
            i += 1
    times = []
    nof_matches = []
    while len(times) in impossibles:
        times.append('-')
        nof_matches.append(0)
    with open('__tmp.cout.broccoli', 'w') as coutfile:
        for line in open('__tmp.broccoli_queries'):
            out = requests.get(broccoli_api + line.strip()
                               + '&format=json&cmd=clearcache').json()
            coutfile.write(str(out))
            times.append(out['result']['time']['total'])
            nof_matches.append(int(out['result']['res']['instances']['sent']))
            while len(times) in impossibles:
                times.append('-')
                nof_matches.append(0)
    queries = []
    for line in open(query_file):
        queries.append(line.strip())
    if len(times) != len(queries) or len(times) != len(nof_matches):
        print('PROBLEM PROCESSING BROCCOLI: q:' + str(len(queries)) + ' t:' +
              str(len(times)) + ' #:' + str(len(nof_matches)), file=sys.stderr)
    return times, nof_matches


def processQueries(query_file, pwd):
    queries = []
    for line in open(query_file):
        queries.append(line.strip())
    bifc_times, bifc_counts = get_virtuoso_bifc_query_times(query_file, pwd)
    bifc_inc_times, bifc_inc_counts = get_virtuoso_bifc_inc_query_times(
        query_file, pwd)
    rdf3x_times, rdf3x_counts = get_rdf3X_query_times(query_file)
    my_times, my_counts = get_my_query_times(query_file)
    broccoli_times, broccoli_counts = get_broccoli_query_times(query_file)
    return queries, bifc_times, bifc_counts, bifc_inc_times, bifc_inc_counts, \
        rdf3x_times, rdf3x_counts, my_times, my_counts, \
        broccoli_times, broccoli_counts


def print_result_table(queries, bifc_times, bifc_counts,
                       bifc_inc_times, bifc_inc_counts,
                       rdf3x_times, rdf3x_counts, my_times, my_counts,
                       broccoli_times, broccoli_counts):
    print("\t".join(['id', 'query', 'bifc', 'bifc_inc', 'rdf3x', 'mine', 'broccoli']))
    print("\t".join(['---', '---', '---', '---', '---', '---', '---']))
    for i in range(0, len(queries)):
        print(
            "\t".join([queries[i], bifc_times[i], bifc_inc_times[i],
                       rdf3x_times[i], my_times[i], broccoli_times[i]]))
        if bifc_counts[i] != rdf3x_counts[i] or bifc_counts[i] != \
                bifc_inc_counts[i] or bifc_counts[i] != my_counts[i] \
                or bifc_counts[i] != broccoli_counts[i]:
            print('DIFFERENT COUNTS FOR QUERY: ' + queries[i] + ': ' +
                  str(bifc_counts[i]) + ' vs ' + str(bifc_inc_counts[i])
                  + ' vs ' + str(rdf3x_counts[i]) + ' vs '+ str(my_counts[i])
                  + ' vs '+ str(broccoli_counts[i]),
                  file=sys.stderr)


def main():
    args = vars(parser.parse_args())
    queries = args['queryfile']
    queries, bifc_times, bifc_counts, bifc_inc_times, bifc_inc_counts, \
        rdf3x_times, rdf3x_counts, my_times, my_counts, \
    broccoli_times, broccoli_counts = processQueries(
            queries, args['virtuoso_pwd'])
    print_result_table(queries,
                       bifc_times, bifc_counts,
                       bifc_inc_times, bifc_inc_counts,
                       rdf3x_times, rdf3x_counts,
                       my_times, my_counts,
                       broccoli_times, broccoli_counts)


if __name__ == '__main__':
    main()
