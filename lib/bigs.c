#include "common.h"
#include "obscure.h"
#include "hash.h"
#include "linefile.h"
#include "localmem.h"
#include "sqlNum.h"
#include "sig.h"
#include "dystring.h"
#include "basicBed.h"
#include "bigBed.h"
#include "bigWig.h"
#include "bigs.h"

#include <limits.h>
#include <math.h>

#define NANUM sqrt(-1)

enum wigOutType get_wig_out_type(char *option)
/* scan the option and return it */
{
    char *wig_type = NULL;
    if (!option)
	wig_type = "fix";
    else 
	wig_type = option;
    if (sameString("bg", wig_type))
	return bedGraphOut;
    else if (sameString("fix", wig_type))
	return fixStepOut;
    else if (sameString("var", wig_type))
	return varStepOut; 
    return invalidOut;
}

int perBaseWigLabelCmp(const void *a, const void *b)
/* for sorting after clustering */
{
    const struct perBaseWig *pbw_a = *((struct perBaseWig **)a);
    const struct perBaseWig *pbw_b = *((struct perBaseWig **)b);    
    int diff = pbw_a->label - pbw_b->label;
    if (diff == 0)
	diff = strcmp(pbw_a->chrom, pbw_b->chrom);
    if (diff == 0)
	diff = pbw_a->chromStart - pbw_b->chromStart;
    return diff;
}

struct perBaseWig *alloc_perBaseWig(char *chrom, int start, int end)
/* simply allocate the perBaseWig. not a super necessary function. */
{
    struct perBaseWig *pbw;
    int size = end - start;
    AllocVar(pbw);
    pbw->chrom = cloneString(chrom);
    pbw->chromStart = start;
    pbw->chromEnd = end;
    pbw->len = size;
    AllocArray(pbw->data, size);
    return pbw;
}

struct perBaseWig *perBaseWigClone(struct perBaseWig *pbw)
/* Clone the structure exactly */
{
    struct perBaseWig *clone;
    int size = pbw->chromEnd - pbw->chromStart;
    int i;
    struct bed *pbw_bed;
    AllocVar(clone);
    clone->chrom = cloneString(pbw->chrom);
    clone->chromStart = pbw->chromStart;
    clone->chromEnd = pbw->chromEnd;
    AllocArray(clone->data, size);
    for (i = 0; i < size; i++)
	clone->data[i] = pbw->data[i];
    clone->subsections = NULL;
    for (pbw_bed = pbw->subsections; pbw_bed != NULL; pbw_bed = pbw_bed->next)
    {
	struct bed *newbed;
	AllocVar(newbed);
	newbed->chrom = cloneString(pbw_bed->chrom);
	newbed->chromStart = pbw_bed->chromStart;
	newbed->chromEnd = pbw_bed->chromEnd;
	slAddHead(&clone->subsections, newbed);
    }
    slReverse(&clone->subsections);
    return clone;
}

void perBaseWigFree(struct perBaseWig **pRegion)
/* Free-up a perBaseWig */
{
    struct perBaseWig *pbw = *pRegion;
    if (!pRegion || !pbw)
	return;
    if (pbw->subsections)
	bedFreeList(&pbw->subsections);
    freeMem(pbw->chrom);
    freez(&pbw->data);
    freez(pRegion);
}

void perBaseWigFreeList(struct perBaseWig **pList)
/* Free up a list of perBaseWigs */
{
    struct perBaseWig *reg;
    while ((reg = slPopHead(pList)) != NULL)
	perBaseWigFree(&reg);
    freez(pList);
}

struct perBaseWig *perBaseWigLoadContinue(struct bbiFile *bbi, char *chrom, int start, int end)
/* load a perBaseWig from a wig/bigWig that's already open */
{
    struct perBaseWig *list = NULL;
    struct lm *lm = lmInit(0);
    struct bbiInterval *intervals = bigWigIntervalQuery(bbi, chrom, start, end, lm);
    struct bbiInterval *bbStart = intervals, *bbEnd;
    while (bbStart != NULL)
    {
	struct perBaseWig *region;
	struct bbiInterval *cur;
	int size = 0;
	int i = 0;
	bbEnd = bbStart;
	/* loop until discontinuity detected */
	while ((bbEnd->next != NULL) && (bbEnd->end == bbEnd->next->start))
	    bbEnd = bbEnd->next;
	AllocVar(region);
	size = bbEnd->end - bbStart->start;
	AllocArray(region->data, size);
	region->chrom = cloneString(chrom);
	region->chromStart = bbStart->start;
	region->chromEnd = bbEnd->end;
	region->len = size;
	for (cur = bbStart; cur != bbEnd->next; cur = cur->next)
	{
	    int j;
	    for (j = cur->start; j < cur->end; j++)
		region->data[i++] = cur->val;
	}
	slAddHead(&list, region);
	bbStart = bbEnd->next;
    }
    lmCleanup(&lm);
    slReverse(&list);
    return list;
}

struct perBaseWig *perBaseWigLoad(char *wigFile, char *chrom, int start, int end)
/* Load all the regions from a wig or bigWig into a list of arrays basically. */
{
    struct bbiFile *bbi = bigWigFileOpen(wigFile);
    struct perBaseWig *list;
    list = perBaseWigLoadContinue(bbi, chrom, start, end);
    bigWigFileClose(&bbi);
    return list;
}

static void chromOob(struct metaBig *mb, char *chrom, int *start, int *end)
{
    int csize = hashIntVal(mb->chromSizeHash, chrom);
    if (*start < 0)
	*start = 0;
    if (*end > csize)
	*end = csize;
}

struct perBaseWig *perBaseWigLoadSingleContinue(struct metaBig *mb, char *chrom, 
						int start, int end, boolean reverse)
/* Load all the regions into one perBaseWig, but with gaps filled  */
/* in with NA value */
{
    struct perBaseWig *list;
    struct perBaseWig *region;
    struct perBaseWig *wholething = NULL;
    double na = NANUM;
    int size = end - start;
    int i, j;
    int s = start, e = end;
    chromOob(mb, chrom, &s, &e);
    list = perBaseWigLoadContinue(mb->big.bbi, chrom, s, e);
    AllocVar(wholething);
    wholething->chrom = cloneString(chrom);
    wholething->chromStart = start;
    wholething->chromEnd = end;
    AllocArray(wholething->data, size);
    wholething->len = size;
    for (i = 0; i < size; i++)
	wholething->data[i] = na;
    if (list)
    {
	for (region = list; region != NULL; region = region->next)
	{
	    int offset = region->chromStart - wholething->chromStart;
	    for (j = 0; j < region->chromEnd - region->chromStart; j++)
		wholething->data[offset + j] = region->data[j];
	}
	perBaseWigFreeList(&list);
    }
    if (reverse)
    {
	double swap;
	for (i = 0; i < (size/2); i++)
	{
	    j = (size-1)-i;
	    swap = wholething->data[i];
	    wholething->data[i] = wholething->data[j];
	    wholething->data[j] = swap;
	}
    }
    return wholething;
}

struct perBaseWig *perBaseWigLoadSingle(char *wigFile, char *chrom, int start, int end, boolean reverse)
/* Load all the regions into one perBaseWig, but with gaps filled  */
/* in with NA value */
{
    struct metaBig *mb = metaBigOpen(wigFile, NULL);
    struct perBaseWig *list;
    list = perBaseWigLoadSingleContinue(mb, chrom, start, end, reverse);
    metaBigClose(&mb);
    return list;
}

struct perBaseMatrix *load_perBaseMatrix(struct metaBig *mb, struct bed6 *regions)
/* loading a wig matrix from a metaBig with regions all the same size.  It should be noted */
/* that the regions may include negative and out-of-bounds coordinates.  Out-of-bounds data is */
/* NA in the matrix. */ 
{
    struct perBaseMatrix *pmat;
    struct bed6 *item;
    int i;
    if (!mb->sections)
	return NULL;
    item = regions;
    AllocVar(pmat);
    pmat->nrow = slCount(regions);
    pmat->ncol = item->chromEnd - item->chromStart;
    AllocArray(pmat->array, pmat->nrow);
    AllocArray(pmat->matrix, pmat->nrow);
    for (i = 0; (i < pmat->nrow) && (item != NULL); i++, item = item->next)
    {
	struct perBaseWig *pbw;
	pbw = perBaseWigLoadSingleContinue(mb, item->chrom, item->chromStart, item->chromEnd, item->strand[0]=='-');
	pbw->name = cloneString(item->name);
	pbw->score = item->score;
	pbw->len = pmat->ncol;
	pbw->strand[0] = item->strand[0];
	pmat->array[i] = pbw;
	pmat->matrix[i] = pbw->data;
    }
    return pmat;
}

void perBaseMatrixAddOrigRegions(struct perBaseMatrix *pbm, struct bed6 *orig_regions)
/* add the original bed information for retrieval later.  it's really important */
/* that this is in the same order as the pbws in the array  */
{
    struct bed6 *region;
    int i;
    int num_regions = slCount(orig_regions);
    if (pbm->nrow != num_regions)
	errAbort("something's wrong");
    for (i = 0, region = orig_regions; ((i < pbm->nrow) && (region != NULL)); i++, region = region->next)
	pbm->array[i]->orig_bed = region;
}

void free_perBaseMatrix(struct perBaseMatrix **ppMat)
/* freeing up a wig matrix */
{
    struct perBaseMatrix *pmat = *ppMat;
    int i;
    for (i = 0; i < pmat->nrow; i++)
    {
	struct perBaseWig *pbw = pmat->array[i];
	perBaseWigFree(&pbw);
    }
    freeMem(pmat->array);
    freeMem(pmat->matrix);
    freez(ppMat);
}

static void wigOutJustVal(FILE *output, int decimals, double val)
{
    int dec = 3;
    if (decimals >= 0)
	dec = decimals;
    if (isnan(val))
	fprintf(output, "NA\n");
    else
	fprintf(output, "%0.*f\n", dec, val);
}

static void perBaseWigOutSection(struct perBaseWig *pbw, FILE *output, enum wigOutType wot,
				 int start, int end, int decimals, boolean nohead, boolean condense)
/* Simply output wigs in different ways.  */
{
    int i;
    if (wot == bedGraphOut)
    {
	if (condense)
	{
	    int j;
	    i = start;
	    while (i < end)
	    {
		j = i+1;
		while ((j < end) && (pbw->data[i] == pbw->data[j]))
		    j++;
		fprintf(output, "%s\t%d\t%d\t", pbw->chrom, pbw->chromStart + i, pbw->chromStart + j);
		wigOutJustVal(output, decimals, pbw->data[i]);
		i = j;
	    }
	}
	else
	{
	    for (i = start; i < end; i++)
	    {
		fprintf(output, "%s\t%d\t%d\t", pbw->chrom, pbw->chromStart + i, pbw->chromStart + i + 1);
		wigOutJustVal(output, decimals, pbw->data[i]);
	    }
	}
    }	    
    else if (wot == varStepOut)
    {
	if (!nohead)
	    fprintf(output, "variableStep chrom=%s span=1\n", pbw->chrom);
	for (i = start; i < end; i++)
	{
	    fprintf(output, "%d\t", pbw->chromStart + i + 1);
	    wigOutJustVal(output, decimals, pbw->data[i]);
	}
    }
    else if (wot == fixStepOut)
    {
	if (!nohead)
	    fprintf(output, "fixedStep chrom=%s start=%d step=1 span=1\n", pbw->chrom, pbw->chromStart + start + 1);
	for (i = start; i < end; i++)
	    wigOutJustVal(output, decimals, pbw->data[i]);
    }
    else
	errAbort("internal error: bad type of wig specified for output");
}
		

void perBaseWigOutput(struct perBaseWig *pbwList, FILE *output, enum wigOutType wot, 
		      int decimals, char *track_line, boolean nohead, boolean condense)
/* Output wiggle. */ 
{
    struct perBaseWig *pbw;
    if (track_line && !nohead)
	fprintf(output, "%s\n", track_line);
    for (pbw = pbwList; pbw != NULL; pbw = pbw->next)
    {
	if (pbw->subsections)
	{
	    struct bed *bed;
	    for (bed = pbw->subsections; bed != NULL; bed = bed->next)
		perBaseWigOutSection(pbw, output, wot, bed->chromStart, bed->chromEnd, decimals, nohead, condense); 
	}
	else
	    perBaseWigOutSection(pbw, output, wot, 0, pbw->chromEnd-pbw->chromStart, decimals, nohead, condense);
    }
}

static void perBaseWigOutSectionNASkip(struct perBaseWig *pbw, FILE *output, enum wigOutType wot, int decimals, int start, int end,
				       boolean nohead, boolean condense)
/* Repeatedly outputs a section while skipping over NA data. */
{
    int i = start;
    while (i < end)
    {
	int s = 0;
	int e = 0;
	/* skip NA data */
	while ((i < end) && (isnan(pbw->data[i])))
	    i++;
	s = i;
	/* keep actual data */
	while ((i < end) && (!isnan(pbw->data[i])))
	    i++;
	e = i;
	if (s < end)
	    perBaseWigOutSection(pbw, output, wot, s, e, decimals, nohead, condense);
    }
}

void perBaseWigOutputNASkip(struct perBaseWig *pbwList, FILE *output, enum wigOutType wot, 
			    int decimals, char *track_line, boolean nohead, boolean condense)
/* Output wiggle skipping a specific value designated as NA. */ 
{
    struct perBaseWig *pbw;
    if (track_line && !nohead)
	fprintf(output, "%s\n", track_line);
    for (pbw = pbwList; pbw != NULL; pbw = pbw->next)
    {
	if (pbw->subsections)
	{
	    struct bed *bed;
	    for (bed = pbw->subsections; bed != NULL; bed = bed->next)
		perBaseWigOutSectionNASkip(pbw, output, wot, decimals, bed->chromStart, 
					   bed->chromEnd, nohead, condense); 
	}
	else
	    perBaseWigOutSectionNASkip(pbw, output, wot, decimals, 0, pbw->chromEnd-pbw->chromStart, nohead, condense);
    }
}

static int pos_compare(const void *a, const void *b)
/* used by qsort for the starts */
{
    return *(int *)a - *(int *)b;
}

static int pos_compare2(const void *a, const void *b)
/* used by qsort for the middles. sorts tuples */
{
    int *pA = (int *)a;
    int *pB = (int *)b;
    return pA[0] - pB[0];
}

static int unique_nums(const int *array, int size)
{
    int i;
    int unique = 0;
    int last = -1;
    for (i = 0; i < size; i++)
    {
	if (array[i] != last)
	    unique++;
	last = array[i];
    }
    return unique;
}

static int unique_nums2(const int *array, int size)
{
    int i;
    int unique = 0;
    int last = -1;
    for (i = 0; i < size * 2; i += 2)
    {
	if (array[i] != last)
	    unique++;
	last = array[i];
    }
    return unique;
}

static void fold_array(int *big_array, int big_array_size, int *unique_array, int *counts_array)
/* convert array of starts into unique array of starts + array of counts */
{
    int i = 0; 
    int j = -1;
    while (i < big_array_size)
    {
	j++;
	unique_array[j] = big_array[i];
	counts_array[j] = 0;
	while ((i < big_array_size) && (big_array[i] == unique_array[j]))
	{
	    i++;
	    counts_array[j]++;
	}
    }
}

static void fold_array2(int *big_array, int big_array_size, int *unique_array, int *counts_array, int *ave_sizes)
/* convert array of starts into unique array of starts + array of counts */
{
    int i = 0; 
    int j = -1;
    while (i < big_array_size)
    {
	j++;
	unique_array[j] = big_array[i];
	counts_array[j] = 0;
	while ((i < big_array_size) && (big_array[i] == unique_array[j]))
	{
	    counts_array[j]++;
	    ave_sizes[j] += big_array[i+1];
	    i += 2;
	}
	ave_sizes[j] /= counts_array[j];
    }
}

void debug_print_middles_array(struct middles *mids)
{
    int i;
    for (i = 0; i < mids->num_mids; i++)
	uglyf("pos[%d] = %d\tcounts[%d] = %d\tsizes[%d] = %d\n", i, mids->mids[i], i, mids->counts[i], i, mids->ave_sizes[i]);
    }

struct middles *beds_to_middles(struct bed6 *bedList)
/* minimal information for stacking middles */
{
    struct bed6 *cur;
    struct middles *mids;
    int *pos_mids;
    int size = slCount(bedList);
    int i = 0;
    if (size == 0)
	return NULL;
    AllocArray(pos_mids, size * 2);
    for (cur = bedList; (i < size) && (cur != NULL); i++, cur = cur->next)
    {	
	pos_mids[i*2] = (cur->chromStart + cur->chromEnd)/2;
	pos_mids[i*2 + 1] = cur->chromEnd - cur->chromStart;
	/* uglyf("%d\t%d\n", pos_mids[i*2], pos_mids[i*2+1]); */
	/* note: can be out-of-range positions. we don't deal with this here */
    }
    qsort(pos_mids, size, sizeof(int) * 2, pos_compare2);
    AllocVar(mids);
    mids->num_mids = unique_nums2(pos_mids, size);
    AllocArray(mids->mids, mids->num_mids);
    AllocArray(mids->counts, mids->num_mids);
    AllocArray(mids->ave_sizes, mids->num_mids);
    fold_array2(pos_mids, size * 2, mids->mids, mids->counts, mids->ave_sizes);
    freeMem(pos_mids);
    return mids;
}

static struct starts *beds_to_starts(struct bed6 *bedList)
/* convert bigBedIntervals to minimal information needed for phasing */
{
    struct bed6 *cur;
    struct starts *starts;
    int *pos_starts;
    int *neg_starts;
    int total_size = slCount(bedList);
    int num_ps = 0;
    int num_ns = 0;
    /* first build arrays to hold all the starts and make sure it's sorted */
    /* on the minus strand */
    if (total_size == 0)
	return NULL;
    AllocArray(pos_starts, total_size);
    AllocArray(neg_starts, total_size);
    for (cur = bedList; cur != NULL; cur = cur->next)
    {
	if (cur->strand[0] == '+')
	    pos_starts[num_ps++] = cur->chromStart;
	else
	    neg_starts[num_ns++] = cur->chromEnd;
    }
    qsort(neg_starts, num_ns, sizeof(int), pos_compare);
    /* find the number of unique starts */
    AllocVar(starts);
    starts->num_pos_starts = unique_nums(pos_starts, num_ps);
    starts->num_neg_starts = unique_nums(neg_starts, num_ns);
    /* init arrays */
    if (starts->num_pos_starts > 0)
    {
	AllocArray(starts->pos_starts, starts->num_pos_starts);
	AllocArray(starts->pos_counts, starts->num_pos_starts);
    }
    if (starts->num_neg_starts > 0)
    {
	AllocArray(starts->neg_starts, starts->num_neg_starts);
	AllocArray(starts->neg_counts, starts->num_neg_starts);
    }
    /* convert the starts into unique starts with counts */
    fold_array(pos_starts, num_ps, starts->pos_starts, starts->pos_counts);
    fold_array(neg_starts, num_ns, starts->neg_starts, starts->neg_counts);
    freeMem(pos_starts);
    freeMem(neg_starts);
    return starts;
}

struct starts *metaBig_get_starts(struct metaBig *mb, char *chrom, unsigned start, unsigned end)
/* return starts struct used with the phasogram/distogram programs */
{
    struct starts *starts = NULL;
    struct lm *lm = lmInit(0);
    struct bed6 *bedList = metaBigBed6Fetch(mb, chrom, start, end, lm);
    starts = beds_to_starts(bedList);
    lmCleanup(&lm);
    return starts;
}

struct middles *metaBig_get_middles(struct metaBig *mb, char *chrom, unsigned start, unsigned end)
/* return middles struct used with stackreads program */
{
    struct middles *mids = NULL;
    struct lm *lm = lmInit(0);
    struct bed6 *bedList = metaBigBed6Fetch(mb, chrom, start, end, lm);
    mids = beds_to_middles(bedList);
    lmCleanup(&lm);
    return mids;
}

static boolean local_file(char *filename)
/* return TRUE if the file is avialable locally */
{
    char *s = cloneString(filename);
    char *colon = strchr(s, ':');
    boolean ret = FALSE;
    if (colon)
	*colon = '\0';
    if (fileExists(s))
	ret = TRUE;
    freeMem(s);
    return ret;
}

struct metaBig *metaBigOpen_favs(char *filename, char *sectionsBed, char *favs_file)
/* open the file based on favorites.txt */
{
    struct metaBig *mb = NULL;
    struct hash *hash = NULL;
    char *fav;
    /* check that the file isn't remote or local */
    if (strstr(filename, "tp://") || local_file(filename))
	mb = metaBigOpen(filename, sectionsBed);
    else
    {
	hash = favs_load_hash(favs_file);
	fav = (char *)hashFindVal(hash, filename);
	if (fav)
	    mb = metaBigOpen(fav, sectionsBed);
	else
	    mb = NULL;
    }
    hashFree(&hash);
    return mb;
}

struct bbiFile *bigWigFileOpen_favs(char *filename, char *favs_file)
/* open the file based on favorites.txt */
{
    struct bbiFile *bbi = NULL;
    struct hash *hash = NULL;
    char *fav;
    /* check that the file isn't remote or local */
    if (strstr(filename, "tp://") || local_file(filename))
	bbi = bigWigFileOpen(filename);
    else
    {
	hash = favs_load_hash(favs_file);
	fav = (char *)hashFindVal(hash, filename);
	if (fav)
	    bbi = bigWigFileOpen(fav);
	else
	    bbi = NULL;
    }
    hashFree(&hash);
    return bbi;
}

static void startsFree(struct starts **pStarts)
/* free a starts */
{
    struct starts *starts = *pStarts;
    freeMem(starts->pos_starts);
    freeMem(starts->neg_starts);
    freez(pStarts);
}

void startsFreeList(struct starts **pList)
/* free a list of starts */
{
    struct starts *reg;
    while ((reg = slPopHead(pList)) != NULL)
	startsFree(&reg);
    freez(pList);
}

void middlesFreeList(struct middles **pList)
/* free a list of middles */
{
    struct middles *mid;
    while ((mid = slPopHead(pList)) != NULL)
    {
	freeMem(mid->mids);
	freeMem(mid->counts);
	freez(&mid);
    }
    freez(pList);
}