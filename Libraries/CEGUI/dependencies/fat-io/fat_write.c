//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//					        FAT16/32 File IO Library
//								    V2.6
// 	  							 Rob Riglar
//						    Copyright 2003 - 2010
//
//   					  Email: rob@robriglar.com
//
//								License: GPL
//   If you would like a version with a more permissive license for use in
//   closed source commercial applications please contact me for details.
//-----------------------------------------------------------------------------
//
// This file is part of FAT File IO Library.
//
// FAT File IO Library is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// FAT File IO Library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with FAT File IO Library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
#include <string.h>
#include "fat_defs.h"
#include "fat_access.h"
#include "fat_table.h"
#include "fat_write.h"
#include "fat_string.h"
#include "fat_misc.h"

#ifdef FATFS_INC_WRITE_SUPPORT
//-----------------------------------------------------------------------------
// fatfs_add_free_space: Allocate another cluster of free space to the end
// of a files cluster chain.
//-----------------------------------------------------------------------------
int fatfs_add_free_space(struct fatfs *fs, UINT32 *startCluster)
{
	UINT32 nextcluster;

	// Set the next free cluster hint to unknown
	if (fs->next_free_cluster != FAT32_LAST_CLUSTER)
		fatfs_set_fs_info_next_free_cluster(fs, FAT32_LAST_CLUSTER); 

	// Start looking for free clusters from the beginning
	if (fatfs_find_blank_cluster(fs, fs->rootdir_first_cluster, &nextcluster))
	{
		// Point last to this
		fatfs_fat_set_cluster(fs, *startCluster, nextcluster);
		
		// Point this to end of file
		fatfs_fat_set_cluster(fs, nextcluster, FAT32_LAST_CLUSTER);

		// Adjust argument reference
		*startCluster = nextcluster;

		return 1;
	}
	else
		return 0;
}
//-----------------------------------------------------------------------------
// fatfs_allocate_free_space: Add an ammount of free space to a file either from
// 'startCluster' if newFile = false, or allocating a new start to the chain if
// newFile = true.
//-----------------------------------------------------------------------------
int fatfs_allocate_free_space(struct fatfs *fs, int newFile, UINT32 *startCluster, UINT32 size)
{
	UINT32 clusterSize;
	UINT32 clusterCount;
	UINT32 nextcluster;

	if (size==0)
		return 0;

	// Set the next free cluster hint to unknown
	if (fs->next_free_cluster != FAT32_LAST_CLUSTER)
		fatfs_set_fs_info_next_free_cluster(fs, FAT32_LAST_CLUSTER); 

	// Work out size and clusters
	clusterSize = fs->sectors_per_cluster * FAT_SECTOR_SIZE;
	clusterCount = (size / clusterSize);

	// If any left over
	if (size-(clusterSize*clusterCount))
		clusterCount++;

	// Allocated first link in the chain if a new file
	if (newFile)
	{
		if (!fatfs_find_blank_cluster(fs, fs->rootdir_first_cluster, &nextcluster))
			return 0;

		// If this is all that is needed then all done
		if (clusterCount==1)
		{
			fatfs_fat_set_cluster(fs, nextcluster, FAT32_LAST_CLUSTER);
			*startCluster = nextcluster;
			return 1;
		}
	}
	// Allocate from end of current chain (startCluster is end of chain)
	else
		nextcluster = *startCluster;

	while (clusterCount)
	{
		if (!fatfs_add_free_space(fs, &nextcluster))
			return 0;

		clusterCount--;
	}

	return 1;
}
//-----------------------------------------------------------------------------
// fatfs_find_free_dir_offset: Find a free space in the directory for a new entry 
// which takes up 'entryCount' blocks (or allocate some more)
//-----------------------------------------------------------------------------
static int fatfs_find_free_dir_offset(struct fatfs *fs, UINT32 dirCluster, int entryCount, UINT32 *pSector, unsigned char *pOffset)
{
	struct fat_dir_entry *directoryEntry;
	unsigned char item=0;
	UINT16 recordoffset = 0;
	unsigned char i=0;
	int x=0;
	int possible_spaces = 0;
	int start_recorded = 0;

	// No entries required?
	if (entryCount == 0)
		return 0;

	// Main cluster following loop
	while (TRUE)
	{
		// Read sector
		if (fatfs_sector_reader(fs, dirCluster, x++, FALSE)) 
		{
			// Analyse Sector
			for (item = 0; item < FAT_DIR_ENTRIES_PER_SECTOR; item++)
			{
				// Create the multiplier for sector access
				recordoffset = FAT_DIR_ENTRY_SIZE * item;

				// Overlay directory entry over buffer
				directoryEntry = (struct fat_dir_entry*)(fs->currentsector.sector+recordoffset);

				// LFN Entry
				if (fatfs_entry_lfn_text(directoryEntry))
				{
					// First entry?
					if (possible_spaces == 0)
					{
						// Store start
						*pSector = x-1;
						*pOffset = item;
						start_recorded = 1;
					}

					// Increment the count in-case the file turns 
					// out to be deleted...
					possible_spaces++;
				}
				// SFN Entry
				else 
				{
					// Has file been deleted?
					if (fs->currentsector.sector[recordoffset] == FILE_HEADER_DELETED)
					{
						// First entry?
						if (possible_spaces == 0)
						{
							// Store start
							*pSector = x-1;
							*pOffset = item;
							start_recorded = 1;
						}

						possible_spaces++;

						// We have found enough space?
						if (possible_spaces >= entryCount)
							return 1;

						// Else continue counting until we find a valid entry!
					}
					// Is the file entry empty?
					else if (fs->currentsector.sector[recordoffset] == FILE_HEADER_BLANK)
					{
						// First entry?
						if (possible_spaces == 0)
						{
							// Store start
							*pSector = x-1;
							*pOffset = item;
							start_recorded = 1;
						}

						// Increment the blank entries count
						possible_spaces++;

						// We have found enough space?
						if (possible_spaces >= entryCount)
							return 1;
					}
					// File entry is valid
					else
					{
						// Reset all flags
						possible_spaces = 0;
						start_recorded = 0;
					}
				}
			} // End of for
		} // End of if
		// Run out of free space in the directory, allocate some more
		else
		{
			UINT32 newCluster;

			// Get a new cluster for directory
			if (!fatfs_find_blank_cluster(fs, fs->rootdir_first_cluster, &newCluster))
				return 0;

			// Add cluster to end of directory tree
			if (!fatfs_fat_add_cluster_to_chain(fs, dirCluster, newCluster))
				return 0;

			// Erase new directory cluster
			memset(fs->currentsector.sector, 0x00, FAT_SECTOR_SIZE);
			for (i=0;i<fs->sectors_per_cluster;i++)
			{
				if (!fatfs_write_sector(fs, newCluster, i, 0))
					return 0;
			}

			// If non of the name fitted on previous sectors
			if (!start_recorded) 
			{
				// Store start
				*pSector = (x-1);
				*pOffset = 0;
				start_recorded = 1;
			}

			return 1;
		}
	} // End of while loop

	return 0;
}
//-----------------------------------------------------------------------------
// fatfs_add_file_entry: Add a directory entry to a location found by FindFreeOffset
//-----------------------------------------------------------------------------
int fatfs_add_file_entry(struct fatfs *fs, UINT32 dirCluster, char *filename, char *shortfilename, UINT32 startCluster, UINT32 size, int dir)
{
	unsigned char item=0;
	UINT16 recordoffset = 0;
	unsigned char i=0;
	UINT32 x=0;
	int entryCount;
	struct fat_dir_entry shortEntry;
	int dirtySector = FALSE;

	UINT32 dirSector = 0;
	unsigned char dirOffset = 0;
	int foundEnd = FALSE;

	unsigned char checksum;
	unsigned char *pSname;

	// No write access?
	if (!fs->disk_io.write_sector)
		return 0;

#if FATFS_INC_LFN_SUPPORT
	// How many LFN entries are required?
	// NOTE: We always request one LFN even if it would fit in a SFN!
	entryCount = fatfs_lfn_entries_required(filename);
	if (!entryCount)
		return 0;
#else
	entryCount = 0;	
#endif

	// Find space in the directory for this filename (or allocate some more)
	// NOTE: We need to find space for at least the LFN + SFN (or just the SFN if LFNs not supported).
	if (!fatfs_find_free_dir_offset(fs, dirCluster, entryCount + 1, &dirSector, &dirOffset))
		return 0;

	// Generate checksum of short filename
	pSname = (unsigned char*)shortfilename;
	checksum = 0;
	for (i=11; i!=0; i--) checksum = ((checksum & 1) ? 0x80 : 0) + (checksum >> 1) + *pSname++;

	// Start from current sector where space was found!
	x = dirSector;

	// Main cluster following loop
	while (TRUE)
	{
		// Read sector
		if (fatfs_sector_reader(fs, dirCluster, x++, FALSE)) 
		{
			// Analyse Sector
			for (item = 0; item < FAT_DIR_ENTRIES_PER_SECTOR; item++)
			{
				// Create the multiplier for sector access
				recordoffset = FAT_DIR_ENTRY_SIZE * item;

				// If the start position for the entry has been found
				if (foundEnd==FALSE)
					if ( (dirSector==(x-1)) && (dirOffset==item) )
						foundEnd = TRUE;

				// Start adding filename
				if (foundEnd)
				{				
					if (entryCount==0)
					{
						// Short filename
						fatfs_sfn_create_entry(shortfilename, size, startCluster, &shortEntry, dir);
						memcpy(&fs->currentsector.sector[recordoffset], &shortEntry, sizeof(shortEntry));

						// Writeback
						return fs->disk_io.write_sector(fs->currentsector.address, fs->currentsector.sector);
					}
#if FATFS_INC_LFN_SUPPORT
					else
					{
						entryCount--;

						// Copy entry to directory buffer
						fatfs_filename_to_lfn(filename, &fs->currentsector.sector[recordoffset], entryCount, checksum); 
						dirtySector = TRUE;
					}
#endif
				}
			} // End of if

			// Write back to disk before loading another sector
			if (dirtySector)
			{
				if (!fs->disk_io.write_sector(fs->currentsector.address, fs->currentsector.sector))
					return 0;

				dirtySector = FALSE;
			}
		} 
		else
			return 0;
	} // End of while loop

	return 0;
}
#endif
