/* bwtool_fill - code adding filler data to  */

#include "common.h"
#include "linefile.h"
#include "hash.h"
#include "options.h"
#include "sqlNum.h"
#include "basicBed.h"
#include "bigWig.h"
#include "bigs.h"
#include "bwtool.h"

void usage_fill()
/* Explain usage and exit. */
{
errAbort(
  "bwtool fill - fill given sections or whole chromosomes with\n"
  "   a given value anywhere there is no data.\n"
  "usage:\n"
  "   bwtool fill <val> input.bw[:chr:start-end] output.wig\n" 
  );
}

void bwtool_fill(struct hash *options, char *favorites, char *regions, unsigned decimals, enum wigOutType wot,
		 boolean condense, char *val_s, char *bigfile, char *outputfile)
/* bwtool_fill - main for filling program */
{
    double val = sqlDouble(val_s);
    struct metaBig *mb = metaBigOpen_favs(bigfile, regions, favorites);
    FILE *out = mustOpen(outputfile, "w");
    struct bed *section;
    int i;
    for (section = mb->sections; section != NULL; section = section->next)
    {
	struct perBaseWig *pbw = perBaseWigLoadSingleContinue(mb, section->chrom, section->chromStart, 
							      section->chromEnd, FALSE);
	for (i = 0; i < pbw->len; i++)
	    if (isnan(pbw->data[i]))
		pbw->data[i] = val;
	perBaseWigOutput(pbw, out, wot, decimals, NULL, FALSE, condense);
	perBaseWigFree(&pbw);
    }
    metaBigClose(&mb);
    carefulClose(&out);
}